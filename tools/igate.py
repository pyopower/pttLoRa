#!/usr/bin/env python3
"""igate.py — publica en APRS-IS las posiciones que se oyen en la red PTT LoRa.

    ./igate.py --conf igate.conf

POR QUE AQUI Y NO EN LA APP NI EN LA CELDA
------------------------------------------
La pasarela hacia Internet es INFRAESTRUCTURA, y por tres razones que no son de
comodidad:

  * **La regla del proyecto es que manda la radio.** Si cada movil publicase su
    posicion directamente en APRS-IS, esa posicion no habria pasado nunca por
    RF: saldrias en el mapa aunque tu nodo no alcance a nadie, y el mapa
    enseñaria cobertura que no existe. Colgado del reflector, para salir en
    APRS hay que **haber llegado por radio a una celda**. Que un nodo no
    aparezca es informacion, no un fallo.
  * **Aqui se ve la red entera.** Por el reflector pasan las balizas de todos:
    celdas, nodos con movil y **nodos sin movil ninguno** —un T-Beam con su GPS,
    o la propia celda—, que son justo los que no podrian publicarse solos.
  * **Una sola contraseña.** Un igate entra con SU login y retransmite lo de los
    demas bajo el indicativo de cada uno (`qAO`). En la app habria que teclear
    el passcode en cada movil, para siempre.

Y no dentro del firmware de la celda porque APRS-IS es una sesion TCP con login,
latidos y reintentos: eso, en un ESP32 con la ranura de OTA contada, se pagaria
en cada celda que se ponga. Aqui cuesta cero de flash y cero de aire.

LA LISTA DE NODOS ES BLANCA, Y ES A PROPOSITO
---------------------------------------------
Solo se publica lo que este en `[nodos]` del fichero de configuracion. Dos
motivos: **los indicativos se repiten** —todos los nodos de un operador llevan
el suyo, asi que el SSID hay que decidirlo a mano y no puede salir de un hash
que ademas pisaria estaciones APRS que ya existan— y **aprs.fi es publico e
historico para siempre**, asi que nadie debe acabar ahi por descuido.
"""
import configparser
import math
import os
import socket
import sys
import threading
import time

FEND, FESC, TFEND, TFESC = 0xC0, 0xDB, 0xDC, 0xDD
PROTO_MAGIC = 0xA1
CAB_LEN = 8
T_INICIO, T_HOLA = 2, 4
CMD_AIRE = 0x11
HOLA_REPETIDOR, HOLA_PUENTE, HOLA_CELDA, HOLA_POS = 0x01, 0x02, 0x04, 0x08

# Identificador de programa en APRS ("tocall"). APZ = experimental / sin
# registrar, que es lo que corresponde a algo propio: PTL por PTT LoRa.
TOCALL = 'APZPTL'

# Cortesia con la red APRS: una baliza nuestra sale cada minuto y eso ahi es
# ruido. Se manda al moverse de verdad, y si no, cada diez minutos para que la
# estacion no desaparezca del mapa.
CADA_S = 600
MOVIDO_M = 100
MINIMO_S = 60


def desentrama(buf, resto):
    """KISS -> lista de (tipo, carga). Devuelve tambien lo que quede a medias."""
    salida = []
    datos = resto + buf
    while True:
        i = datos.find(bytes([FEND]))
        if i < 0:
            return salida, b''
        j = datos.find(bytes([FEND]), i + 1)
        if j < 0:
            return salida, datos[i:]
        trama, datos = datos[i + 1:j], datos[j:]
        out = bytearray()
        esc = False
        for b in trama:
            if esc:
                out.append(FEND if b == TFEND else (FESC if b == TFESC else b))
                esc = False
            elif b == FESC:
                esc = True
            else:
                out.append(b)
        if out:
            salida.append((out[0], bytes(out[1:])))


def passcode(indicativo):
    """El passcode de APRS-IS. No es un secreto: sale del propio indicativo, y
    lo unico que dice es "afirmo ser esta estacion". Se calcula aqui para que
    no haya que ir a buscarlo, pero se puede poner a mano en el fichero."""
    ind = indicativo.split('-')[0].upper()
    h = 0x73E2
    for i in range(0, len(ind), 2):
        h ^= ord(ind[i]) << 8
        if i + 1 < len(ind):
            h ^= ord(ind[i + 1])
    return h & 0x7FFF


def lee_baliza(t):
    """Trama T_HOLA del aire -> lo que lleva dentro, o None.

    ⚠️ LA POSICION SE CORTA POR EL FINAL, NO PARTIENDO POR CEROS. Son 8 bytes
    binarios y cualquiera puede valer cero: una longitud de 1,5 grados es
    15.000.000 = C0 E1 E4 00. Partir toda la cola por `\\0` —que es lo natural
    despues de haberlo hecho con el nombre— la trocea. Ver protocolo.h."""
    if len(t) < CAB_LEN + 2 or t[0] != PROTO_MAGIC:
        return None
    tipo = t[2] >> 4
    if tipo not in (T_HOLA, T_INICIO):
        return None
    src = (t[3] << 16) | (t[4] << 8) | t[5]
    if tipo == T_INICIO:
        # ⚠️ AQUI ES DONDE VIENE LA POSICION DE UNA PERSONA (fw v1.35). La
        # baliza ciclica solo la lleva si es una CELDA; quien la recibe de su
        # movil la manda al abrir el PTT, que es cuando se identifica de todas
        # formas. Cuerpo: [modo][indicativo]\0[lat][lon].
        flags, bateria = 0, 0
        cuerpo = t[CAB_LEN + 1:]
    else:
        flags, bateria = t[CAB_LEN], t[CAB_LEN + 1]
        cuerpo = t[CAB_LEN + 2:]
    lat = lon = None
    # En el INICIO no hay flags: la posicion esta si hay cola detras del cero.
    hay = (flags & HOLA_POS) if tipo == T_HOLA else (0 in cuerpo)
    if hay and len(cuerpo) >= 9:
        lat = int.from_bytes(cuerpo[-8:-4], 'little', signed=True) / 1e7
        lon = int.from_bytes(cuerpo[-4:], 'little', signed=True) / 1e7
        cuerpo = cuerpo[:-9]                     # y su separador
    trozos = cuerpo.split(b'\0')
    ind = trozos[0].decode('ascii', 'ignore').strip()
    nombre = trozos[1].decode('ascii', 'ignore').strip() if len(trozos) > 1 else ''
    # En el INICIO no viaja el nombre del cacharro, solo el indicativo de quien
    # habla. La lista blanca se busca igual: primero por nombre, luego por src.
    return dict(src=src, flags=flags, bateria=bateria, indicativo=ind,
                nombre=nombre if tipo == T_HOLA else '', lat=lat, lon=lon)


def grados_aprs(v, largo, borroso=0):
    """Grados decimales -> DDMM.mm de APRS. El formato son GRADOS Y MINUTOS
    pegados, no grados decimales: la confusion clasica, y equivocarse aqui deja
    la estacion a decenas de kilometros con una pinta perfectamente creible.

    `borroso` es la AMBIGUEDAD DE POSICION de APRS, que no es un redondeo
    nuestro sino parte del formato: se mandan los ultimos digitos EN BLANCO y
    quien recibe SABE QUE NO LOS SABE — los clientes pintan un circulo de
    incertidumbre en vez de un punto. Es la diferencia entre "esta por aqui" y
    "esta exactamente aqui", que ademas seria mentira.

        0  4230.38N   exacta         (~18 m)
        1  4230.3 N   decimas        (~185 m)
        2  4230.  N   minuto entero  (~1,4 km a nuestra latitud)   <- "1 km"
        3  423 .  N   diez minutos   (~18 km)
        4  42  .  N   grado entero

    El punto decimal y el hemisferio NO se tocan: el campo tiene que conservar
    su longitud exacta o deja de ser un paquete valido."""
    hemi = ('S' if v < 0 else 'N') if not largo else ('W' if v < 0 else 'E')
    v = abs(v)
    g = int(v)
    m = (v - g) * 60.0
    ancho = 3 if largo else 2
    txt = '%0*d%05.2f' % (ancho, g, m)
    # De derecha a izquierda y saltandose el punto: centesimas, decimas,
    # unidades de minuto, decenas de minuto.
    huecos = [len(txt) - 1, len(txt) - 2, len(txt) - 4, len(txt) - 5]
    t = list(txt)
    for i in range(min(borroso, 4)):
        t[huecos[i]] = ' '
    return ''.join(t) + hemi


def metros(a, b):
    """Distancia aproximada. Sobra para decidir si alguien se ha movido."""
    (la1, lo1), (la2, lo2) = a, b
    dx = (lo2 - lo1) * 111320.0 * math.cos(math.radians((la1 + la2) / 2))
    dy = (la2 - la1) * 110540.0
    return math.hypot(dx, dy)


class Igate:
    def __init__(self, cfg):
        self.cfg = cfg
        self.llamada = cfg['aprs']['llamada'].strip().upper()
        pw = cfg['aprs'].get('passcode', 'auto').strip()
        self.passcode = passcode(self.llamada) if pw in ('', 'auto') else int(pw)
        self.servidor = cfg['aprs'].get('servidor', 'rotate.aprs2.net').strip()
        self.puerto = int(cfg['aprs'].get('puerto', '14580'))
        # ⚠️ POR DEFECTO NO ENVIA. Publicar en APRS-IS no se puede deshacer:
        # aprs.fi guarda el historico para siempre. Asi se puede ver exactamente
        # que saldria antes de que salga.
        self.enviar = cfg['aprs'].get('enviar', 'no').strip().lower() in ('si', 'sí', 'yes', '1')
        self.comentario = cfg['aprs'].get(
            'comentario', '434.400MHz Voice over LoRa').strip()
        # Que se le pega detras al comentario. Es una lista y no dos
        # interruptores porque lo que de verdad se administra aqui es un
        # PRESUPUESTO DE CARACTERES: el dia que en `comentario` entre una URL,
        # esto se vacia y ya esta.
        self.extras = cfg['aprs'].get('extras', 'nombre bateria').split()
        # EL ESTADO: la linea que dice QUE ES ESTO Y DONDE MIRARLO.
        #
        # Va en una trama APRS de estado (`>`) y no pegada al comentario de
        # posicion, y esa es la clave: el comentario solo garantiza 43
        # caracteres y ahi ya va la frecuencia, que es el dato util para quien
        # quiera escuchar. El estado tiene su propio hueco (62), asi que la URL
        # no le quita el sitio a nada. Es lo mismo que hace WPSD con su
        # "Powered by WPSD (https://wpsd.radio)".
        #
        # Sin esto, una celda aparece en aprs.fi entre miles de estaciones y
        # nadie puede saber que hay detras ni como montarse una.
        self.estado = cfg['aprs'].get(
            'estado', 'PTT LoRa: voz Codec2 por radio '
                      'github.com/pyopower/pttLoRa').strip()
        # Cada cuanto se repite. No va con cada baliza: una linea que no cambia
        # repetida cada minuto es ruido en APRS-IS y no aporta nada.
        self.estado_cada = int(cfg['aprs'].get('estado_cada', '1800'))
        self.ultimo_estado = {}          # indicativo APRS -> cuando se mando
        # `nombre = INDICATIVO [borroso N]`. Ver la nota de `grados_aprs`.
        self.nodos = {}
        for k, v in (cfg['nodos'].items() if 'nodos' in cfg else []):
            trozos = v.split()
            amb = 0
            if 'borroso' in trozos:
                i = trozos.index('borroso')
                amb = int(trozos[i + 1]) if i + 1 < len(trozos) else 2
            self.nodos[k.strip().lower()] = (trozos[0].upper(), amb)
        self.ultimas = {}          # indicativo APRS -> (lat, lon, cuando)
        # src -> nombre del cacharro, aprendido de las BALIZAS. Hace falta
        # porque el INICIO —donde viene la posicion de una persona— solo lleva
        # el indicativo de quien habla, y la lista blanca va por nombre. Como
        # cada nodo baliza cada minuto, para cuando alguien transmite ya se sabe
        # quien es. Y si no se supiera, queda el src en hexadecimal.
        self.nombres = {}
        self.desconocidos = set()
        self.aprs = None
        self.lock = threading.Lock()

    # ------------------------------------------------------------- APRS-IS --
    def _conecta_aprs(self):
        if not self.enviar:
            return None
        s = socket.create_connection((self.servidor, self.puerto), 20)
        s.settimeout(20)
        bienvenida = s.recv(512).decode('ascii', 'replace').strip()
        # `filter t/m` = no queremos que nos manden nada; esto solo publica.
        s.sendall(('user %s pass %d vers pttlora-igate 1.0 filter t/m\r\n'
                   % (self.llamada, self.passcode)).encode())
        resp = s.recv(512).decode('ascii', 'replace').strip()
        log('APRS-IS %s:%d -> %s | %s' % (self.servidor, self.puerto,
                                          bienvenida, resp))
        if 'unverified' in resp.lower():
            log('⚠ el servidor dice UNVERIFIED: el passcode no cuadra con %s, '
                'y todo lo que se mande se tirara en silencio' % self.llamada)
        s.settimeout(None)
        return s

    def manda_aprs(self, linea):
        if not self.enviar:
            log('(solo mirar) %s' % linea)
            return
        with self.lock:
            try:
                if self.aprs is None:
                    self.aprs = self._conecta_aprs()
                self.aprs.sendall((linea + '\r\n').encode('ascii', 'replace'))
                log('-> %s' % linea)
            except Exception as e:
                log('APRS-IS caido (%s); se reintenta en la siguiente' % e)
                try:
                    self.aprs.close()
                except Exception:
                    pass
                self.aprs = None

    # -------------------------------------------------------- las balizas --
    def baliza(self, b):
        if b['lat'] is None:
            return
        if b['nombre']:
            self.nombres[b['src']] = b['nombre']
        # LA LISTA BLANCA SE BUSCA POR TRES SITIOS, Y HACE FALTA.
        #
        # El `src` de una transmision NO ES EL DEL NODO: el firmware lo saca del
        # hash del INDICATIVO de quien habla (`src_de` -> `hash_indicativo` en
        # main.cpp), mientras que el de una baliza sale de la MAC de la placa.
        # O sea que `C31AG` transmitiendo es `56d666`, que no se parece en nada
        # al `09dd2e` con el que baliza el nodo por el que ha salido.
        #
        # Ahi estaba lo que fallaba el 9-sep: se buscaba SOLO por nombre del
        # cacharro, y el nombre solo viaja en las balizas. Una transmision no lo
        # lleva —lleva el indicativo de la PERSONA— asi que no encontraba nada y
        # no publicaba nunca. Es lo coherente con la v1.35: desde que la
        # posicion de una persona va en el INICIO, lo que hay que identificar es
        # a la persona, no a la placa por la que ha salido.
        claves = [(b['nombre'] or self.nombres.get(b['src'], '')).lower(),
                  b['indicativo'].lower(),
                  '%06x' % b['src']]
        destino, borroso = next(
            (self.nodos[k] for k in claves if k and k in self.nodos), (None, 0))
        if not destino:
            if clave not in self.desconocidos:
                self.desconocidos.add(clave)
                log('sin publicar: %s (%s, src %06x) no esta en [nodos]'
                    % (b['nombre'] or '?', b['indicativo'], b['src']))
            return

        ahora = time.time()
        ant = self.ultimas.get(destino)
        if ant:
            lejos = metros((ant[0], ant[1]), (b['lat'], b['lon']))
            if ahora - ant[2] < MINIMO_S:
                return
            if lejos < MOVIDO_M and ahora - ant[2] < CADA_S:
                return
        self.ultimas[destino] = (b['lat'], b['lon'], ahora)

        celda = bool(b['flags'] & HOLA_CELDA)
        # ⚠️ EL SIMBOLO YA DICE EL PAPEL. `#` es un digipetidor y `[` es una
        # persona: en el mapa se ven distintos de un vistazo y no hace falta
        # ademas la palabra. Estaba escrita y se quito — en un comentario de
        # 43 caracteres, repetir en letra lo que ya dice el icono es gastar el
        # sitio que necesita la URL del proyecto.
        simbolo = '#' if celda else '['       # digi / persona
        # EL COMENTARIO ES LO UNICO QUE VE UN DESCONOCIDO.
        #
        # En aprs.fi esta estacion aparece entre miles, y sin explicacion nadie
        # sabe que es: ni que hay VOZ ahi, ni en que frecuencia, ni con que
        # ajustes tendria que ponerse para oirla. Por eso el comentario dice
        # las tres cosas.
        #
        # Y la frecuencia va LA PRIMERA y con el formato exacto `434.400MHz`
        # -sin espacio antes de MHz- porque esa es la convencion de APRS para
        # un dato de sintonia: asi aprs.fi y los clientes la sacan como campo
        # propio en vez de como texto suelto. Escrita de cualquier otra manera
        # sigue leyendose, pero deja de ser un dato y pasa a ser una frase.
        com = self.comentario
        if 'nombre' in self.extras and (b['nombre'] or b['indicativo']):
            com += ' ' + (b['nombre'] or b['indicativo'])
        if 'bateria' in self.extras and b['bateria']:
            com += ' bat %d%%' % b['bateria']
        # ASCII a secas: por APRS-IS viajan bytes de 8 bits, pero un acento en
        # un comentario es la clase de cosa que un cliente viejo pinta mal.
        com = com.encode('ascii', 'ignore').decode('ascii')
        if len(com) > 43:
            log('⚠ comentario de %d caracteres: APRS garantiza 43, y hay '
                'clientes que cortan por ahi -> "%s"' % (len(com), com))
        # `qAO` = lo mete una estacion que NO es la que lo dijo. Es lo honesto:
        # esto no es un paquete APRS recibido en RF, es una baliza de otra red
        # traducida aqui.
        paquete = ('%s>%s,TCPIP*,qAO,%s:!%s/%s%s%s'
                  % (destino, TOCALL, self.llamada,
                     grados_aprs(b['lat'], False, borroso),
                     grados_aprs(b['lon'], True, borroso), simbolo, com))
        self.manda_aprs(paquete)
        self.manda_estado(destino, ahora)

    def manda_estado(self, destino, ahora):
        """La trama de estado, de tarde en tarde. Ver `self.estado`."""
        if not self.estado:
            return
        if ahora - self.ultimo_estado.get(destino, 0) < self.estado_cada:
            return
        self.ultimo_estado[destino] = ahora
        txt = self.estado.encode('ascii', 'ignore').decode('ascii')
        if len(txt) > 62:
            log('⚠ estado de %d caracteres: APRS garantiza 62 -> "%s"'
                % (len(txt), txt))
            txt = txt[:62]
        self.manda_aprs('%s>%s,TCPIP*,qAO,%s:>%s'
                        % (destino, TOCALL, self.llamada, txt))

    # ------------------------------------------------------- el reflector --
    def escucha(self, host, puerto):
        while True:
            try:
                s = socket.create_connection((host, puerto), 20)
                s.settimeout(None)
                log('enganchado al reflector %s:%d' % (host, puerto))
                resto = b''
                while True:
                    trozo = s.recv(4096)
                    if not trozo:
                        break
                    tramas, resto = desentrama(trozo, resto)
                    for tipo, carga in tramas:
                        if tipo != CMD_AIRE:
                            continue
                        b = lee_baliza(carga)
                        if b:
                            self.baliza(b)
            except Exception as e:
                log('reflector caido (%s), reintento en 5 s' % e)
            time.sleep(5)


def log(t):
    print('[%s] %s' % (time.strftime('%H:%M:%S'), t), flush=True)


def main():
    conf = 'igate.conf'
    a = sys.argv[1:]
    if '--conf' in a:
        conf = a[a.index('--conf') + 1]
    if not os.path.exists(conf):
        print('no encuentro %s' % conf, file=sys.stderr)
        return 1
    cfg = configparser.ConfigParser()
    cfg.read(conf, encoding='utf-8')
    g = Igate(cfg)
    log('igate de %s (passcode %d), %s'
        % (g.llamada, g.passcode,
           'PUBLICANDO en APRS-IS' if g.enviar else 'en modo SOLO MIRAR'))
    log('nodos publicables: %s' % (', '.join(
        '%s->%s%s' % (k, v[0], ' (borroso %d)' % v[1] if v[1] else '')
        for k, v in g.nodos.items()) or 'ninguno'))
    ref = cfg['reflector']['host'] if 'reflector' in cfg else '127.0.0.1'
    pto = int(cfg['reflector'].get('puerto', '4461')) if 'reflector' in cfg else 4461
    g.escucha(ref, pto)
    return 0


if __name__ == '__main__':
    sys.exit(main())
