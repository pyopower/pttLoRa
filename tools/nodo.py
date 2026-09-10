#!/usr/bin/env python3
# nodo.py — habla con un nodo de PTT LoRa por USB (tramas KISS).
#
#   ./nodo.py --puerto /dev/ttyACM0 estado
#   ./nodo.py --ble AA:BB:CC:11:22:33 estado     (lo mismo por Bluetooth BLE)
#   ./nodo.py --bt AA:BB:CC:11:22:33 estado      (SPP, solo firmware <= 1.25)
#   ./nodo.py --tcp 192.168.4.1 estado           (lo mismo por WiFi, puerto 4460)
#   ./nodo.py --tcp 192.168.4.1 --ident EA3ABC hablar voz.wav
#       El nodo admite VARIOS clientes por WiFi a la vez y cada uno emite con
#       SU indicativo: eso es lo que hace --ident.
#   ./nodo.py config C31AG --canal 1 --saltos 3 [--potencia 17] [--perfil 0|1|2]
#                          perfil: 0=auto  1=repetidor fijo  2=solo mi radio
#   ./nodo.py escuchar [segundos]
#   ./nodo.py hablar <fichero.wav|-> [--modo 1200]      manda voz de verdad
#   ./nodo.py nombre <texto>               nombre del nodo en la lista del movil
#   ./nodo.py pos <lat> <lon> [--viva]     donde esta el nodo (grados decimales)
#   ./nodo.py pos                          olvidarla y dejar de publicarla
#       Por defecto la posicion es FIJA: se guarda en el nodo y sobrevive a un
#       corte de luz. Es la de una celda, que no se mueve. Con `--viva` no se
#       guarda y caduca a la media hora, que es lo que manda un movil con su GPS.
#
# ⚠️ EN EL BANCO, BAJA LA POTENCIA. Con varias placas a menos de un metro y a
#    17 dBm se SATURA el receptor de las vecinas: a 434 MHz, 20 cm son solo unos
#    11 dB de perdida, asi que el de al lado ve +6 dBm — por encima de su
#    saturacion y rozando el maximo absoluto del SX1278 (~+10 dBm). A 5 cm le
#    entran los 17 dBm enteros al LNA y se puede dañar.
#    Sintoma tipico: tramas corruptas (`mal=`) sin motivo aparente.
#      ./nodo.py radio 434.400 --sf 7 --potencia 2      <- para probar en la mesa
#    Y ACORDARSE DE SUBIRLA otra vez antes de instalar el nodo en su sitio: una
#    celda con amplificador necesita los 17 dBm de excitacion, con 2 dBm el
#    amplificador casi no da nada.
#
#   ./nodo.py bt <0|1|2>     0=apagado (placas con la antena BT/WiFi rota),
#                            1=ventana de 5 min tras arrancar, 2=siempre (por
#                            defecto). Tiene efecto al reiniciar el nodo.
#   ./nodo.py mando <host> [puerto]        canal de mando saliente (sin args = soltar)
#   ./nodo.py red2 <ssid> <clave>          segunda red WiFi (sin args = olvidar)
#   ./nodo.py ampli <pin> [--previo 10] [--cola 5] [--preambulo 8]
#                          linea de PTT para un amplificador externo (pin 0 = ninguna)
#   ./nodo.py tono [segundos]                           manda un tono de prueba
#
# Es el mismo papel que hara el movil: el nodo solo mueve bytes, el codec vive
# aqui. Sirve para probar la radio sin app y para el futuro gateway.

import os, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import serial

# El nodo habla exactamente el mismo KISS por USB y por Bluetooth SPP, asi que
# aqui solo cambia el tubo. Es a proposito: la app Android hara esto mismo.
try:
    import socket
except ImportError:
    socket = None

FEND, FESC, TFEND, TFESC = 0xC0, 0xDB, 0xDC, 0xDD

CMD_INICIO, CMD_VOZ, CMD_FIN, CMD_CONFIG, CMD_ESTADO = 1, 2, 3, 4, 5
CMD_CONFIRMA, CMD_IDENT, CMD_RED, CMD_ENLACE = 0x09, 0x0E, 0x0F, 0x10
CMD_RADIO = 0x0A
CMD_AMPLI = 0x14
CMD_RED2  = 0x12
CMD_MANDO = 0x13
CMD_NOMBRE = 0x15
CMD_BT     = 0x16
CMD_POS    = 0x17
POS_VIVA, POS_FIJA = 0, 1
EV = {0x81: 'INICIO', 0x82: 'VOZ', 0x83: 'FIN', 0x84: 'HOLA',
      0x85: 'ESTADO', 0x86: 'CANAL', 0x87: 'EMPAREJA', 0x89: 'PTT',
      0x8A: 'RED', 0x8F: 'LOG'}
PUERTO_APP, PUERTO_ENLACE = 4460, 4461
RED_OFF, RED_CLIENTE, RED_AP = 0, 1, 2

# Modos de Codec2: nombre -> (codigo en la trama, bits por trama, ms por trama)
MODOS = {'3200': (0, 64, 20), '2400': (1, 48, 20), '1600': (2, 64, 40),
         '1300': (3, 52, 40), '1200': (4, 48, 40), '700C': (5, 28, 40)}
LOTE_MS = 480


def enmarcar(tipo, datos=b''):
    out = bytearray([FEND, tipo])
    for b in datos:
        if b == FEND:
            out += bytes([FESC, TFEND])
        elif b == FESC:
            out += bytes([FESC, TFESC])
        else:
            out.append(b)
    out.append(FEND)
    return bytes(out)


class Nodo:
    def __init__(self, puerto=None, baud=115200, bt=None, canal_bt=1, tcp=None,
                 ble=None):
        # Cuatro tubos, el mismo KISS por los cuatro. Es a proposito: la app
        # hace exactamente esto, y el nodo no distingue por donde le hablan
        # salvo para decidir si pide codigo de acceso.
        #
        # ⚠️ `--bt` (SPP) solo sirve con firmware 1.25 o anterior: desde la
        # v1.26 el nodo habla BLE y hay que usar `--ble`. Ver la nota de
        # `main.cpp` sobre por que se cambio.
        if ble:
            from tuboble import TuboBle
            self.s = TuboBle(ble)
            self.bt = True          # mismo camino de lectura/escritura
        elif tcp:
            host, _, pto = tcp.partition(':')
            self.s = socket.create_connection((host, int(pto or PUERTO_APP)), 5)
            self.s.settimeout(None)
            self.bt = True          # mismo camino de lectura/escritura
        elif bt:
            self.s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM,
                                   socket.BTPROTO_RFCOMM)
            self.s.connect((bt, canal_bt))
            # Bloqueante para ENVIAR (un timeout corto aqui rompe los envios
            # grandes en cuanto el nodo tarda en drenar) y `select` para leer,
            # que es lo que de verdad necesita un plazo corto.
            self.s.settimeout(None)
            self.bt = True
        else:
            self.s = serial.Serial(puerto, baud, timeout=0.1)
            self.bt = False
        time.sleep(0.3)
        self.buf = bytearray()
        self.dentro = False
        self.escape = False

    def _escribe(self, datos):
        if self.bt:
            self.s.sendall(datos)
        else:
            self.s.write(datos)
            self.s.flush()

    def _lee_bloque(self):
        if not self.bt:
            return self.s.read(512)
        # El tubo BLE ya trae su propia espera con plazo: no hay descriptor que
        # meter en un `select`.
        if hasattr(self.s, 'conectado'):
            return self.s.recv(4096)
        import select
        try:
            listo, _, _ = select.select([self.s], [], [], 0.2)
            return self.s.recv(4096) if listo else b''
        except (TimeoutError, OSError):
            return b''

    def manda(self, tipo, datos=b''):
        self._escribe(enmarcar(tipo, datos))

    def lee(self, segundos):
        """Devuelve (tipo, payload) de cada trama que llegue."""
        t0 = time.time()
        while time.time() - t0 < segundos:
            d = self._lee_bloque()
            if not d:
                continue
            for c in d:
                if c == FEND:
                    if self.dentro and len(self.buf) >= 1:
                        yield self.buf[0], bytes(self.buf[1:])
                    self.dentro, self.escape = True, False
                    self.buf = bytearray()
                    continue
                if not self.dentro:
                    continue
                if c == FESC:
                    self.escape = True
                    continue
                if self.escape:
                    c = FEND if c == TFEND else FESC
                    self.escape = False
                self.buf.append(c)


def pinta(tipo, p):
    nombre = EV.get(tipo, hex(tipo))
    if tipo in (0x85, 0x8F):
        print('  %-7s %s' % (nombre, p.decode('utf-8', 'replace')))
    elif tipo == 0x86:
        print('  %-7s %s' % (nombre, 'ocupado' if p and p[0] else 'libre'))
    elif tipo == 0x81 and len(p) >= 7:
        # Desde el fw v1.35, detras del indicativo puede venir `\0` + 8 bytes
        # con la POSICION de quien habla: la de una persona sale AQUI, en el
        # inicio de cada transmision, y no en la baliza ciclica. Se corta por el
        # final como en la baliza, que los 8 bytes son binarios y pueden llevar
        # ceros dentro.
        cola = p[7:]
        donde = ''
        if len(cola) >= 9 and 0 in cola:
            la = int.from_bytes(cola[-8:-4], 'little', signed=True) / 1e7
            lo = int.from_bytes(cola[-4:], 'little', signed=True) / 1e7
            donde = ' %.6f,%.6f' % (la, lo)
            cola = cola[:-9]
        print('  %-7s rssi=%d snr=%d src=%s stream=%d modo=%d %s%s'
              % (nombre, int.from_bytes(p[0:1], 'big', signed=True),
                 int.from_bytes(p[1:2], 'big', signed=True), p[2:5].hex(),
                 p[5], p[6], cola.split(b'\0')[0].decode('ascii', 'replace'),
                 donde))
    elif tipo == 0x82 and len(p) >= 8:
        print('  %-7s rssi=%d snr=%d src=%s stream=%d seq=%d modo=%d n=%d (%d B)'
              % (nombre, int.from_bytes(p[0:1], 'big', signed=True),
                 int.from_bytes(p[1:2], 'big', signed=True), p[2:5].hex(),
                 p[5], p[6], p[7], p[8] if len(p) > 8 else 0, len(p) - 9))
    elif tipo == 0x84 and len(p) >= 9:
        # OJO: [rssi][snr][src x3][stream][flags][bateria][indicativo]\0[nombre]
        # Estaba desplazado un byte y se leia el indicativo con la bateria
        # pegada delante ("HC31AG-9" era 0x48=72% + C31AG-9) y los flags donde
        # estaba el stream. Un decodificador mal alineado no da error: da datos.
        #
        # Desde la v1.30 detras del indicativo va `\0` + el NOMBRE del nodo: los
        # nodos de un mismo operador llevan todos su indicativo, asi que sin el
        # nombre eran tres balizas identicas de `C31AG` sin forma de saber cual
        # es cual. Los firmwares viejos no lo mandan, de ahi el `if`.
        papeles = []
        if p[6] & 0x04: papeles.append('CELDA')
        elif p[6] & 0x01: papeles.append('repetidor')
        if p[6] & 0x02: papeles.append('con movil')
        # Desde la v1.34 puede venir un tercer campo tras otro `\0`: la
        # POSICION, lat y lon como enteros de 4 bytes en diezmillonesimas de
        # grado. Se parte por ceros, asi que los firmwares que no la mandan
        # siguen leyendose igual — y los que la mandan no rompen a nadie que
        # solo mire los dos primeros campos.
        # ⚠️ LA POSICION SE CORTA POR EL FINAL, NO PARTIENDO POR CEROS. Son 8
        # bytes BINARIOS y cualquiera de ellos puede valer cero: una longitud
        # de 1,5 grados es 15.000.000 = C0 E1 E4 00, con su cero dentro. Si se
        # parte la cola entera por `\0` esos 8 bytes se trocean y la posicion
        # sale partida o desaparece. El texto (indicativo y nombre) SI se parte
        # por ceros, porque es texto y no los lleva.
        cuerpo = p[8:]
        donde = ''
        if p[6] & 0x08 and len(cuerpo) >= 9:
            la = int.from_bytes(cuerpo[-8:-4], 'little', signed=True) / 1e7
            lo = int.from_bytes(cuerpo[-4:], 'little', signed=True) / 1e7
            donde = ' %.6f,%.6f' % (la, lo)
            cuerpo = cuerpo[:-9]            # y su separador
        resto = cuerpo.split(b'\0')
        ind = resto[0].decode('ascii', 'replace')
        nom = resto[1].decode('ascii', 'replace') if len(resto) > 1 else ''
        quien = ('%s (%s)' % (nom, ind)) if nom else ind
        print('  %-7s rssi=%d snr=%d src=%s %s bat=%d%% %s%s'
              % (nombre, int.from_bytes(p[0:1], 'big', signed=True),
                 int.from_bytes(p[1:2], 'big', signed=True), p[2:5].hex(),
                 '+'.join(papeles) or 'nodo', p[7], quien, donde))
    else:
        print('  %-7s %s' % (nombre, p.hex()))


def codifica(raw, modo):
    """PCM de 8 kHz/16 bits -> (codigo de modo, tramas por lote, lotes).

    Cada LOTE son 480 ms de voz, que es lo que va en una trama de radio: se
    manda uno cada 480 ms y esa cadencia es la que fija todo el diseño (ver el
    calculo de tiempo en el aire en `bench/malla.py`).

    ⚠️ `c2enc` RELLENA CADA TRAMA HASTA BYTE ENTERO, asi que los bytes por trama
    son `ceil(bits/8)` y no `bits/8`: a 1200 son 48 bits = 6 B justos, pero a
    700C son 28 bits que ocupan 4 B. Suponer lo contrario da lotes de un tamaño
    que no cuadra con nada y voz que se descodifica a ruido.

    El ultimo lote puede salir corto —lo normal, la voz no acaba en un multiplo
    de 480 ms— y se manda igual: el receptor va por el largo, no por el numero
    de tramas anunciado.
    """
    cod, bits, ms = MODOS[modo]
    tramas_por_lote = int(LOTE_MS / ms)
    bytes_por_trama = (bits + 7) // 8
    tam_lote = tramas_por_lote * bytes_por_trama

    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        crudo = os.path.join(tmp, 'v.raw')
        comprimido = os.path.join(tmp, 'v.bin')
        open(crudo, 'wb').write(raw)
        subprocess.run(['c2enc', modo, crudo, comprimido], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        datos = open(comprimido, 'rb').read()

    lotes = [datos[i:i + tam_lote] for i in range(0, len(datos), tam_lote)]
    return cod, tramas_por_lote, lotes


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__ or 'ver la cabecera del fichero')
        return 1

    def opt(nombre, cast, defecto):
        return cast(a[a.index(nombre) + 1]) if nombre in a else defecto

    puerto = opt('--puerto', str, '/dev/ttyACM0')
    bt = opt('--bt', str, '')
    ble = opt('--ble', str, '')
    tcp = opt('--tcp', str, '')
    ident = opt('--ident', str, '')
    pos = [x for i, x in enumerate(a)
           if not x.startswith('--')
           and (i == 0 or not a[i - 1].startswith('--'))]
    cmd = pos[0]
    n = (Nodo(ble=ble) if ble else
         Nodo(tcp=tcp) if tcp else
         Nodo(bt=bt) if bt else
         Nodo(puerto))
    # El tubo se cierra pase lo que pase. Por BLE es OBLIGATORIO: un enlace que
    # queda colgado en BlueZ deja al nodo creyendo que tiene un cliente, y
    # entonces deja de anunciarse y parece averiado (ver `tuboble.py`).
    import atexit as _atexit
    _atexit.register(lambda: getattr(n.s, 'close', lambda: None)())
    # Cada cliente declara SU indicativo nada mas entrar: si no, el nodo emite
    # con el suyo y con varios usuarios eso seria emitir con indicativo ajeno.
    if ident:
        n.manda(CMD_IDENT, ident.upper().encode()[:12])
        time.sleep(0.2)

    if cmd == 'estado':
        n.manda(CMD_ESTADO)
        for t, p in n.lee(2.0):
            pinta(t, p)

    elif cmd == 'config':
        ind = pos[1].upper().encode()[:12]
        canal = opt('--canal', int, 1)
        saltos = opt('--saltos', int, 3)
        pot = opt('--potencia', int, 17)
        # 0=auto (puente+repetidor con supresion), 1=repetidor fijo, 2=solo mi radio
        perfil = opt('--perfil', int, 0)
        n.manda(CMD_CONFIG, bytes([canal, saltos, pot, perfil]) + ind)
        for t, p in n.lee(2.0):
            pinta(t, p)

    elif cmd == 'bt':
        n.manda(CMD_BT, bytes([int(pos[1])]))
        for t, p in n.lee(2.0):
            pinta(t, p)

    elif cmd == 'radio':
        # radio <MHz> [--bw 250] [--sf 7] [--cr 5] [--potencia 17]
        khz = int(round(float(pos[1]) * 1000))
        bw = int(opt('--bw', float, 250))
        n.manda(CMD_RADIO, bytes([khz >> 24 & 255, khz >> 16 & 255,
                                  khz >> 8 & 255, khz & 255,
                                  bw >> 8, bw & 255,
                                  opt('--sf', int, 7), opt('--cr', int, 5),
                                  opt('--potencia', int, 17)]))
        for t, p in n.lee(2.0):
            pinta(t, p)

    elif cmd == 'red':
        # red off | red ap <ssid> <clave> | red cliente <ssid> <clave>
        modo = {'off': RED_OFF, 'cliente': RED_CLIENTE, 'ap': RED_AP}[pos[1]]
        ssid = pos[2].encode() if len(pos) > 2 else b''
        clave = pos[3].encode() if len(pos) > 3 else b''
        n.manda(CMD_RED, bytes([modo]) + ssid + b'\0' + clave)
        for t, p in n.lee(2.0):
            pinta(t, p)

    elif cmd == 'enlace':
        # enlace off | enlace <host> <puerto>
        if pos[1] == 'off':
            n.manda(CMD_ENLACE)
        else:
            pto = int(pos[2]) if len(pos) > 2 else PUERTO_ENLACE
            n.manda(CMD_ENLACE, bytes([pto >> 8, pto & 0xFF]) + pos[1].encode())
        for t, p in n.lee(2.0):
            pinta(t, p)

    elif cmd == 'codigo':
        # Autoriza esta conexion con el codigo de 6 cifras que sale en la
        # pantalla del nodo.
        n.manda(CMD_CONFIRMA, pos[1].encode())
        for t, p in n.lee(2.0):
            pinta(t, p)

    elif cmd == 'escuchar':
        secs = float(pos[1]) if len(pos) > 1 else 30.0
        print('-- escuchando %.0f s' % secs)
        for t, p in n.lee(secs):
            pinta(t, p)

    elif cmd == 'nombre':
        # nombre <texto>   |   nombre        (vuelve al automatico nodo-XXXX)
        # Es como se ve el CACHARRO en la lista de Bluetooth del movil, aparte
        # del indicativo (que identifica la estacion por radio y cambia).
        n.manda(CMD_NOMBRE,
                ' '.join(pos[1:]).encode('utf-8')[:16] if len(pos) > 1 else b'')
        for t, p in n.lee(2.0):
            pinta(t, p)

    elif cmd == 'pos':
        # pos <lat> <lon> [--viva]   |   pos        (olvidarla)
        #
        # Grados decimales aqui, enteros en la trama: se manda en
        # DIEZMILLONESIMAS de grado (1e-7), little-endian y con signo. Al nodo
        # no le llega nunca un numero con coma.
        if len(pos) > 2:
            la = int(round(float(pos[1]) * 1e7))
            lo = int(round(float(pos[2]) * 1e7))
            origen = POS_VIVA if '--viva' in a else POS_FIJA
            n.manda(CMD_POS, bytes([origen]) +
                    la.to_bytes(4, 'little', signed=True) +
                    lo.to_bytes(4, 'little', signed=True))
        else:
            n.manda(CMD_POS, b'')
        for t, p in n.lee(2.0):
            pinta(t, p)

    elif cmd == 'mando':
        # mando <host> [puerto]   |   mando        (para soltarlo)
        # Canal de mando SALIENTE: lo abre el nodo. Es como se administra un
        # nodo en una red donde no se pueden abrir puertos entrantes.
        if len(pos) > 1:
            host = pos[1]
            pto = int(pos[2]) if len(pos) > 2 else 4464
            n.manda(CMD_MANDO, bytes([(pto >> 8) & 0xFF, pto & 0xFF]) + host.encode())
        else:
            n.manda(CMD_MANDO, b'')
        for t, p in n.lee(2.0):
            pinta(t, p)

    elif cmd == 'red2':
        # red2 <ssid> <clave>   |   red2        (para olvidarla)
        if len(pos) > 1:
            ss = pos[1]; cl = pos[2] if len(pos) > 2 else ''
            n.manda(CMD_RED2, ss.encode() + b'\x00' + cl.encode())
        else:
            n.manda(CMD_RED2, b'')
        for t, p in n.lee(1.5):
            pinta(t, p)

    elif cmd == 'ampli':
        # ampli <pin> [--previo 10] [--cola 5] [--preambulo 8]
        # pin 0 = sin linea de PTT (solo preambulo)
        pin = int(pos[1]) if len(pos) > 1 else 0
        n.manda(CMD_AMPLI, bytes([pin & 0xFF,
                                  opt('--previo', int, 10) & 0xFF,
                                  opt('--cola', int, 5) & 0xFF,
                                  opt('--preambulo', int, 8) & 0xFF]))
        for t, p in n.lee(1.5):
            pinta(t, p)

    elif cmd in ('hablar', 'tono'):
        modo = opt('--modo', str, '1200')
        if cmd == 'tono':
            import math, struct
            secs = float(pos[1]) if len(pos) > 1 else 3.0
            raw = b''.join(struct.pack('<h', int(9000 * math.sin(
                2 * math.pi * 700 * i / 8000.0))) for i in range(int(8000 * secs)))
        else:
            f = pos[1]
            tmp = '/tmp/.pttlora_src.raw'
            subprocess.run(['sox', f, '-r', '8000', '-c', '1', '-b', '16',
                            '-e', 'signed-integer', tmp], check=True)
            raw = open(tmp, 'rb').read()
        cod, n_tramas, lotes = codifica(raw, modo)
        print('-- %s: %d lotes de %d B (%.1f s)'
              % (modo, len(lotes), len(lotes[0]) if lotes else 0,
                 len(raw) / 16000.0))
        n.manda(CMD_INICIO, bytes([cod]))
        t0 = time.time()
        for i, lote in enumerate(lotes):
            n.manda(CMD_VOZ, bytes([cod, n_tramas]) + lote)
            # ritmo real: un lote cada 480 ms, como lo haria el movil
            espera = t0 + (i + 1) * LOTE_MS / 1000.0 - time.time()
            if espera > 0:
                time.sleep(espera)
        n.manda(CMD_FIN)
        print('-- soltado tras %d lotes' % len(lotes))
        for t, p in n.lee(1.5):
            pinta(t, p)

    else:
        print('orden desconocida:', cmd)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
