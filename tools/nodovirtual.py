#!/usr/bin/env python3
# nodovirtual.py — un "nodo" de PTT LoRa sin radio, hecho de software.
#
#   ./nodovirtual.py --a 192.168.4.1            se cuelga del enlace de un nodo
#   ./nodovirtual.py --escucha 4461             hace de punto de reunion
#
# Habla el protocolo del ENLACE (puerto 4461): por ahi no van ordenes, van
# TRAMAS DEL AIRE tal cual, envueltas en KISS con el tipo CMD_AIRE.
#
# Sirve para dos cosas:
#   1. Probar el encadenado de tres nodos teniendo solo dos placas.
#   2. Es la base de un REFLECTOR en un servidor con IP alcanzable: los nodos
#      de cada ubicacion se enganchan a el y quedan todos en la misma red, sin
#      que ninguno necesite IP publica ni abrir puertos. Con --escucha ya hace
#      justo eso: reparte a todos menos al que lo trajo.

import socket
import sys
import threading
import time

FEND, FESC, TFEND, TFESC = 0xC0, 0xDB, 0xDC, 0xDD
CMD_AIRE = 0x11
CAB_LEN = 8
T_VOZ, T_INICIO, T_FIN, T_HOLA = 1, 2, 3, 4
NOMBRES = {1: 'VOZ', 2: 'INICIO', 3: 'FIN', 4: 'HOLA'}


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


class Desentrama:
    def __init__(self):
        self.buf = bytearray()
        self.dentro = False
        self.escape = False

    def mete(self, datos):
        """Devuelve las tramas KISS completas que salgan."""
        salida = []
        for c in datos:
            if c == FEND:
                if self.dentro and self.buf:
                    salida.append((self.buf[0], bytes(self.buf[1:])))
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
        return salida


def describe(t):
    """Resume una trama del aire tal como viaja por el enlace."""
    if len(t) < CAB_LEN:
        return 'corta (%d B)' % len(t)
    tipo = t[2] >> 4
    saltos = t[2] & 0x0F
    src = (t[3] << 16) | (t[4] << 8) | t[5]
    cola = ''
    if tipo == T_INICIO and len(t) > CAB_LEN + 1:
        cola = ' ' + t[CAB_LEN + 1:].decode('ascii', 'replace')
    elif tipo == T_HOLA and len(t) > CAB_LEN + 2:
        cola = ' ' + t[CAB_LEN + 2:].decode('ascii', 'replace')
    return '%-6s src=%06x stream=%d seq=%d saltos=%d%s' % (
        NOMBRES.get(tipo, '?%d' % tipo), src, t[6], t[7], saltos, cola)


def cliente(host, puerto, vistas):
    s = socket.create_connection((host, puerto), 5)
    # El plazo es para CONECTAR, no para leer: por un enlace puede no pasar
    # nada durante minutos y eso es lo normal, no un fallo.
    s.settimeout(None)
    print('enganchado al enlace de %s:%d' % (host, puerto))
    d = Desentrama()
    while True:
        b = s.recv(4096)
        if not b:
            print('enlace cerrado')
            return
        for tipo, p in d.mete(b):
            if tipo == CMD_AIRE:
                vistas.append(p)
                print('  <- %s' % describe(p))


# Arbitraje global. Un stream se da por terminado si llega su FIN o si pasan
# estos segundos sin verlo: un nodo puede caerse a media transmision y no se
# puede dejar la red muda esperando un FIN que ya no va a llegar.
MUDO_S = 2.0
TOT_S = 180.0


def reflector(puerto, vistas):
    """Punto de reunion: reparte a todos menos al que lo trajo, y ARBITRA.

    Repartir es EXACTAMENTE lo que hace un nodo con varios enlaces, y la razon
    de que los bucles no sean un problema: el descarte de duplicados de cada
    nodo se encarga de lo que vuelva.

    Arbitrar es lo que un nodo NO puede hacer. Cada nodo bloquea el PTT cuando
    oye el canal ocupado, pero eso solo tapa las colisiones que le da tiempo a
    ver: dos personas en ubicaciones distintas que pulsan a la vez no se enteran
    la una de la otra hasta que la voz ha cruzado. El reflector si las ve a las
    dos, asi que **se queda con la primera y descarta lo que llegue de otras
    hasta que esa termine**. No arregla el retardo, pero convierte una
    transmision destrozada en una que se entiende y otra que no sale."""
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('', puerto))
    srv.listen(32)
    print('reflector escuchando en el puerto %d' % puerto)
    pares = []
    lock = threading.Lock()
    # Quien tiene la palabra: (src, stream, cuando empezo, ultima vez que se vio)
    turno = [None]
    descartadas = [0]

    def deja_pasar(t):
        """Arbitraje: True si esta trama debe repartirse."""
        tipo = t[2] >> 4
        src = (t[3] << 16) | (t[4] << 8) | t[5]
        stream = t[6]
        ahora = time.time()
        q = turno[0]
        # Las balizas no se arbitran: son identificacion de estacion, no voz,
        # y callarlas seria justo lo contrario de lo que hay que hacer.
        if tipo == T_HOLA:
            return True
        if q and (ahora - q[3] > MUDO_S or ahora - q[2] > TOT_S):
            q = turno[0] = None        # se fue sin despedirse, o TOT
        if q is None:
            # Un FIN no coge el turno: es el final de algo, no el principio.
            # Si lo cogiera (pasa cuando el FIN de una transmision pisada llega
            # despues de que la buena termine), dejaria la red muda hasta que
            # caducara — y quien hablara a continuacion no saldria.
            if tipo == T_FIN:
                return True
            turno[0] = [src, stream, ahora, ahora]
            return True
        if q[0] == src and q[1] == stream:
            q[3] = ahora
            if tipo == T_FIN:
                turno[0] = None
            return True
        descartadas[0] += 1
        return False

    def atiende(c, quien):
        d = Desentrama()
        try:
            while True:
                b = c.recv(4096)
                if not b:
                    break
                for tipo, p in d.mete(b):
                    if tipo != CMD_AIRE or len(p) < CAB_LEN:
                        continue
                    vistas.append(p)
                    with lock:
                        pasa = deja_pasar(p)
                    print('  %s %s %s' % (quien, '->' if pasa else 'XX', describe(p)))
                    if not pasa:
                        continue
                    with lock:
                        for otro, _ in pares:
                            if otro is not c:
                                try:
                                    otro.sendall(enmarcar(CMD_AIRE, p))
                                except OSError:
                                    pass
        finally:
            with lock:
                pares[:] = [x for x in pares if x[0] is not c]
            c.close()
            print('%s se fue' % quien)

    while True:
        c, dir_ = srv.accept()
        quien = '%s:%d' % dir_
        print('%s enganchado' % quien)
        with lock:
            pares.append((c, quien))
        threading.Thread(target=atiende, args=(c, quien), daemon=True).start()


def main():
    def opt(nombre, defecto=None):
        return (sys.argv[sys.argv.index(nombre) + 1]
                if nombre in sys.argv else defecto)

    vistas = []
    if '--escucha' in sys.argv:
        # Sin buffer: si corre como servicio, lo que se escribe tiene que
        # aparecer en el registro cuando pasa, no cuando se llene un buffer.
        try:
            sys.stdout.reconfigure(line_buffering=True)
        except AttributeError:
            pass
        reflector(int(opt('--escucha', '4461')), vistas)
        return 0
    host = opt('--a')
    if not host:
        print(__doc__.strip())
        return 1
    segundos = float(opt('--segundos', '0') or 0)
    h = threading.Thread(target=cliente,
                         args=(host, int(opt('--puerto', '4461')), vistas),
                         daemon=True)
    h.start()
    if segundos:
        time.sleep(segundos)
        print('-- %d tramas vistas por el enlace' % len(vistas))
    else:
        h.join()
    return 0


if __name__ == '__main__':
    sys.exit(main())
