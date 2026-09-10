#!/usr/bin/env python3
"""nododatos.py — un NODO SIN RADIO al que las apps se conectan por Internet.

    ./nododatos.py --escucha 4460 --reflector 127.0.0.1:4461

POR QUE EXISTE
--------------
Para que la app tenga un **camino de datos** ademas del de radio, y pueda seguir
hablando cuando la radio no llega. La redundancia no la da un protocolo nuevo:
la da tener DOS caminos hacia la misma red.

El problema es que en PTT LoRa hay **dos protocolos distintos** y no encajaban:

  * El de CLIENTE (puerto 4460): ordenes y eventos. Es lo que habla la app.
        manda   CMD_INICIO [modo] · CMD_VOZ [modo][n][datos] · CMD_FIN
        recibe  EV_INICIO · EV_VOZ · EV_FIN · EV_HOLA
  * El de ENLACE (puerto 4461): **tramas del aire tal cual**, con su cabecera
    de 8 bytes, envueltas en CMD_AIRE. Es lo que hablan los nodos entre si y lo
    que entiende el reflector.

Esto traduce de uno a otro. Para la app es un nodo mas al que conectarse por
TCP —no hay que enseñarle ningun protocolo nuevo— y para el reflector es un
nodo mas de la malla.

    app ──cliente 4460──► nododatos ──enlace 4461──► reflector ──► celdas

⚠️ LO QUE LLEGA POR AQUI SE MARCA CON **RSSI 127**, que es la convencion que ya
usa el firmware: 127 dBm es imposible por radio, asi que es la señal de "esto
vino por Internet, no por el aire". La app lo pinta distinto y el usuario ve por
donde le llega cada cosa.
"""
import os
import socket
import sys
import threading
import time

FEND, FESC, TFEND, TFESC = 0xC0, 0xDB, 0xDC, 0xDD
PROTO_MAGIC = 0xA1
CAB_LEN = 8
T_VOZ, T_INICIO, T_FIN, T_HOLA = 1, 2, 3, 4

CMD_INICIO, CMD_VOZ, CMD_FIN = 0x01, 0x02, 0x03
CMD_ESTADO, CMD_IDENT, CMD_AIRE = 0x05, 0x0E, 0x11
EV_INICIO, EV_VOZ, EV_FIN, EV_HOLA, EV_ESTADO = 0x81, 0x82, 0x83, 0x84, 0x85

RSSI_INTERNET = 127          # ver la nota de arriba
SALTOS = 3


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
    """Desentramado KISS. Un flujo de bytes -> tramas (tipo, carga)."""

    def __init__(self):
        self.buf = bytearray()
        self.dentro = False
        self.escape = False

    def mete(self, datos):
        for c in datos:
            if c == FEND:
                if self.dentro and self.buf:
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


def hash_indicativo(ind):
    """El mismo hash de 24 bits que usa el firmware para el `src` de un CLIENTE.

    ⚠️ Para el src del NODO el firmware usa la MAC (ver `src_de_la_mac`), pero
    para el de un cliente que se identifica usa el hash de su indicativo — que
    es lo que hace falta aqui, porque cada app que se conecta es un cliente.

    ⚠️ Y ESTO ES **FNV-1a**, no un `h*31+c`. Lo fue durante un tiempo y estaba
    mal aunque el comentario dijera lo contrario: con otro hash, la MISMA
    persona entraba en la red con un `src` por radio y otro distinto por el
    camino de datos, que es justo el problema que se estuvo persiguiendo el
    8-sep. `C31AG` es 56d666, no 4644de. Si se toca esto, se toca tambien
    `hash_indicativo` de protocolo.h — son el mismo numero por definicion."""
    h = 2166136261
    for c in ind.encode('ascii', 'ignore'):
        if 0x61 <= c <= 0x7A:           # a-z -> A-Z, como el firmware
            c -= 32
        h ^= c
        h = (h * 16777619) & 0xFFFFFFFF
    return h & 0xFFFFFF


def cabecera(tipo, src, stream, seq, canal=1):
    return bytes([PROTO_MAGIC, canal, ((tipo << 4) | (SALTOS & 0x0F)),
                  (src >> 16) & 0xFF, (src >> 8) & 0xFF, src & 0xFF,
                  stream, seq])


class Puente:
    def __init__(self, reflector, canal=1):
        self.host, _, p = reflector.partition(':')
        self.puerto = int(p or 4461)
        self.canal = canal
        self.clientes = []            # sockets de apps conectadas
        self.quien = {}               # socket -> indicativo que declaro
        self.hablando = {}            # (src, stream) -> indicativo, y cuando
        self.cerrojo = threading.Lock()
        self.enlace = None
        threading.Thread(target=self._al_reflector, daemon=True).start()

    # ------------------------------------------------------- al reflector --
    def _al_reflector(self):
        """Se reconecta sola: un puente que se cae y no vuelve es peor que no
        tenerlo, porque nadie se entera."""
        while True:
            try:
                s = socket.create_connection((self.host, self.puerto), 10)
                s.settimeout(None)
                self.enlace = s
                print('[enlace] conectado al reflector %s:%d'
                      % (self.host, self.puerto), flush=True)
                d = Desentrama()
                while True:
                    trozo = s.recv(4096)
                    if not trozo:
                        break
                    for tipo, carga in d.mete(trozo):
                        if tipo == CMD_AIRE and len(carga) >= CAB_LEN:
                            self._aire_a_clientes(carga)
            except Exception as e:
                print('[enlace] caido (%s), reintento en 5 s' % e, flush=True)
            self.enlace = None
            time.sleep(5)

    def manda_al_aire(self, trama):
        s = self.enlace
        if s is None:
            return False
        try:
            s.sendall(enmarcar(CMD_AIRE, trama))
            return True
        except Exception:
            return False

    # ---------------------------------------------------- a los clientes ---
    def _aire_a_clientes(self, b):
        """Trama del aire -> evento de cliente. Es la traduccion de verdad."""
        if b[0] != PROTO_MAGIC:
            return
        tipo = b[2] >> 4
        src = b[3:6]
        stream, seq = b[6], b[7]
        cuerpo = b[CAB_LEN:]
        r = bytes([RSSI_INTERNET & 0xFF, 0]) + src + bytes([stream])

        if tipo == T_INICIO:
            ev, carga = EV_INICIO, r + cuerpo
            # Quien habla, para no devolverle su propia voz. Ver `_a_todos`.
            self._apunta_quien(src, stream, cuerpo[1:])
        elif tipo == T_VOZ:
            ev, carga = EV_VOZ, r + bytes([seq]) + cuerpo
        elif tipo == T_FIN:
            ev, carga = EV_FIN, src + bytes([stream])
        elif tipo == T_HOLA:
            ev, carga = EV_HOLA, r + cuerpo
        else:
            return
        self._a_todos(enmarcar(ev, carga), no_para=self._de_quien(src, stream))

    # -------------------------------------------------------------- el eco --
    #
    # LO QUE SALE POR RADIO VUELVE POR AQUI. El camino es:
    #
    #   app -> su nodo -> RF -> celda -> enlace -> reflector -> aqui -> la app
    #
    # y esa copia trae el `src` DE LA CELDA, no el del que hablo: la app no la
    # reconoce como suya y la reproduce. Eso es el eco que se oyo el 8-sep-2026
    # al encender el camino de datos.
    #
    # El `src` no sirve para reconocerlo —cada camino sella la voz con la
    # identidad de SU nodo— pero el INDICATIVO si: va en el T_INICIO y lo pone
    # el que habla. Asi que se apunta de quien es cada stream y no se le manda
    # de vuelta a quien lo dijo.
    #
    # Esto no sustituye al filtro de la app (0.9.21), que ademas descarta la
    # copia doble de OTRO; es la misma cura por el otro lado, y hace falta
    # porque protege a los moviles que no se hayan actualizado.
    def _apunta_quien(self, src, stream, ind):
        try:
            q = ind.decode('ascii', 'replace').split('\0')[0].strip()
        except Exception:
            return
        if not q:
            return
        with self.cerrojo:
            self.hablando[(bytes(src), stream)] = (q, time.time())
            # Se limpia por edad: una transmision dura segundos, y sin esto el
            # diccionario crece toda la vida del proceso.
            if len(self.hablando) > 64:
                viejo = time.time() - 120
                for k, v in list(self.hablando.items()):
                    if v[1] < viejo:
                        del self.hablando[k]

    def _de_quien(self, src, stream):
        with self.cerrojo:
            v = self.hablando.get((bytes(src), stream))
        return v[0] if v and time.time() - v[1] < 120 else None

    def _a_todos(self, datos, salvo=None, no_para=None):
        with self.cerrojo:
            for c in list(self.clientes):
                if c is salvo:
                    continue
                # Su propia voz de vuelta, no.
                if no_para and self.quien.get(c, '').upper() == no_para.upper():
                    continue
                try:
                    c.sendall(datos)
                except Exception:
                    self.clientes.remove(c)
                    self.quien.pop(c, None)

    def _echa_fantasmas(self, sock, ind):
        """Cierra las conexiones VIEJAS del mismo indicativo.

        ⚠️ UN MOVIL NO PUEDE TENER TRES CONEXIONES, y las tenia: cuando cambia
        de red o se rompe el tunel, el socket viejo NO se cierra —TCP no se
        entera de que al otro lado ya no hay nadie— y aqui se seguia dando por
        vivo. Visto el 10-sep-2026: `dentro (3 en total)` con un solo movil.

        Y no es solo contabilidad. El reparto local excluia unicamente el socket
        por el que se hablaba, asi que **a tus propios fantasmas se les mandaba
        tu voz**; si alguno seguia llegando al aparato, te oias a ti mismo. Es
        el mismo fallo que el relevo de mando tenia por la mañana, en otro
        sitio: identificarse tiene que echar al de antes.

        Quien quiera dos aparatos a la vez tiene los SSID (`C31AG-2`), que para
        eso estan: con indicativo distinto, ninguno echa al otro."""
        if not ind.strip():
            return
        fuera = []
        with self.cerrojo:
            for c in list(self.clientes):
                if c is not sock and self.quien.get(c, '').upper() == ind.strip().upper():
                    fuera.append(c)
                    self.clientes.remove(c)
                    self.quien.pop(c, None)
        for c in fuera:
            print('[cliente] fantasma de %s: se cierra la conexion vieja' % ind,
                  flush=True)
            try:
                c.close()
            except Exception:
                pass

    # -------------------------------------------------------- un cliente --
    def atiende(self, sock, direccion):
        with self.cerrojo:
            self.clientes.append(sock)
        print('[cliente] %s dentro (%d en total)'
              % (direccion[0], len(self.clientes)), flush=True)
        ind = 'ANON'
        src = hash_indicativo(ind)
        stream, seq = int(time.time()) & 0xFF, 0
        d = Desentrama()
        try:
            while True:
                trozo = sock.recv(4096)
                if not trozo:
                    break
                for tipo, carga in d.mete(trozo):
                    if tipo == CMD_IDENT and carga:
                        ind = carga.decode('ascii', 'replace')[:12]
                        src = hash_indicativo(ind)
                        with self.cerrojo:
                            self.quien[sock] = ind.strip()
                        print('[cliente] %s es %s' % (direccion[0], ind), flush=True)
                        self._echa_fantasmas(sock, ind)
                    elif tipo == CMD_INICIO and carga:
                        stream = (stream + 1) & 0xFF
                        seq = 0
                        # El indicativo se cierra con un CERO, igual que en
                        # el firmware (v1.42): un byte que impide que nada se
                        # lea pegado detras. Ver la nota de CMD_INICIO en
                        # main.cpp — `C31AG` llego a la red como `C31AGG`.
                        t = cabecera(T_INICIO, src, stream, 0, self.canal) + \
                            bytes([carga[0]]) + ind.encode('ascii', 'ignore') + b'\0'
                        self.manda_al_aire(t)
                        self._a_todos(enmarcar(EV_INICIO,
                            bytes([RSSI_INTERNET, 0]) + t[3:6] + bytes([stream]) +
                            t[CAB_LEN:]), salvo=sock, no_para=ind)
                    elif tipo == CMD_VOZ and len(carga) >= 2:
                        seq = (seq + 1) & 0xFF
                        t = cabecera(T_VOZ, src, stream, seq, self.canal) + carga
                        self.manda_al_aire(t)
                        self._a_todos(enmarcar(EV_VOZ,
                            bytes([RSSI_INTERNET, 0]) + t[3:6] + bytes([stream, seq]) +
                            carga), salvo=sock, no_para=ind)
                    elif tipo == CMD_FIN:
                        seq = (seq + 1) & 0xFF
                        t = cabecera(T_FIN, src, stream, seq, self.canal)
                        self.manda_al_aire(t)
                        self._a_todos(enmarcar(EV_FIN,
                            t[3:6] + bytes([stream])), salvo=sock, no_para=ind)
                    elif tipo == CMD_ESTADO:
                        txt = ('v-datos %s perfil=nodo-de-datos canal=%d '
                               'enlace=%s clientes=%d ptt=libre'
                               % (ind, self.canal,
                                  'en pie' if self.enlace else 'caido',
                                  len(self.clientes)))
                        sock.sendall(enmarcar(EV_ESTADO, txt.encode('utf-8')))
        except Exception:
            pass
        finally:
            with self.cerrojo:
                if sock in self.clientes:
                    self.clientes.remove(sock)
                self.quien.pop(sock, None)
            try:
                sock.close()
            except Exception:
                pass
            print('[cliente] %s fuera' % direccion[0], flush=True)


def main():
    a = sys.argv[1:]

    def opt(n, d):
        return a[a.index(n) + 1] if n in a else d

    escucha = int(opt('--escucha', '4460'))
    reflector = opt('--reflector', '127.0.0.1:4461')
    canal = int(opt('--canal', '1'))

    p = Puente(reflector, canal)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('0.0.0.0', escucha))
    srv.listen(8)
    print('nodo de datos escuchando en %d, reflector en %s' % (escucha, reflector),
          flush=True)
    while True:
        sock, dirn = srv.accept()
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        threading.Thread(target=p.atiende, args=(sock, dirn), daemon=True).start()


if __name__ == '__main__':
    main()
