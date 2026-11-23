#!/usr/bin/env python3
"""pttlora_asl.py - puente entre un nodo ASL privado (USRP) y la red PTT LoRa.

    nodo ASL 1934 --USRP 8 kHz--> este puente --cliente 4460, Codec2 1200--> PTT LoRa

Para la red PTT LoRa es un cliente mas del camino de datos, como la app sin
placa: se identifica con un indicativo propio (p. ej. "C31AG-60 AllStar") y
manda/recibe lotes de 480 ms de Codec2 1200. Para AllStar es un nodo privado
con radio USRP: lo que sale del nodo es "lo que hay que transmitir" y lo que
se le manda con keyup=1 es "lo que se recibe".

Conectar/desconectar NO se hace aqui: se enlaza o desenlaza el nodo privado
desde el 60596 (*31934 / *11934, Allmon3 o Mobile ASL Client). Sin enlace,
lo que llega de LoRa muere en el 1934 y por el 1934 no sale nada.

Salvaguardas: TOT de AllStar hacia LoRa (el aire a 1200 bps se satura con un
TG o un hub activo), semiduplex (mientras uno habla no entra el otro) y ceder
el turno si la red PTT LoRa se lo da a otro.
"""
import configparser
import ctypes
import os
import queue
import re
import socket
import struct
import sys
import threading
import time

# ------------------------------------------------------------------ config --
cfg = configparser.ConfigParser()
cfg.read(sys.argv[1] if len(sys.argv) > 1 else '/etc/pttlora-asl.ini')
C = cfg['puente'] if 'puente' in cfg else {}
USRP_ESCUCHA = (C.get('usrp_escucha_ip', '172.18.0.1'), int(C.get('usrp_escucha_puerto', 34104)))
USRP_DESTINO = (C.get('usrp_destino_ip', '172.18.0.2'), int(C.get('usrp_destino_puerto', 32104)))
SERVIDOR = (C.get('servidor', 'or.adan.ovh'), int(C.get('puerto', 4460)))
INDICATIVO = C.get("indicativo", "C31AG-60 ASL")[:12]
TOT_S = float(C.get('tot', 60))
GANANCIA_A_LORA = float(C.get('ganancia_a_lora', 1.0))
GANANCIA_A_ASL = float(C.get('ganancia_a_asl', 1.0))
# Quien habla en ADN: lo apunta el MMDVM_Bridge de DVSwitch en su log ("received
# network voice header from EA3XYZ to TG 213"). Vacio = no se busca.
ORIGEN_LOG = C.get('origen_log', '/var/log/dvswitch/MMDVM_Bridge-adn-%Y-%m-%d.log')
ORIGEN_RED = C.get('origen_red', 'ADN')


def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)


# ----------------------------------------------------------------- codec2 --
class Codec2:
    MODE_1200 = 5            # codec2.h: 3200=0 2400=1 1600=2 1400=3 1300=4 1200=5

    def __init__(self):
        for n in ('libcodec2.so.1.0', 'libcodec2.so.1.1', 'libcodec2.so.1.2', 'libcodec2.so'):
            try:
                self.lib = ctypes.CDLL(n)
                break
            except OSError:
                continue
        else:
            raise SystemExit('falta libcodec2 (apt install libcodec2-1.0)')
        L = self.lib
        L.codec2_create.restype = ctypes.c_void_p
        L.codec2_create.argtypes = [ctypes.c_int]
        for f in ('codec2_samples_per_frame', 'codec2_bits_per_frame'):
            getattr(L, f).argtypes = [ctypes.c_void_p]
        L.codec2_encode.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_short)]
        L.codec2_decode.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_short), ctypes.c_char_p]
        self.enc = L.codec2_create(self.MODE_1200)
        self.dec = L.codec2_create(self.MODE_1200)
        self.n = L.codec2_samples_per_frame(self.enc)               # 320
        self.nb = (L.codec2_bits_per_frame(self.enc) + 7) // 8      # 6
        assert (self.n, self.nb) == (320, 6), (self.n, self.nb)

    def codifica(self, pcm):          # 320 int16 -> 6 bytes
        buf = (ctypes.c_short * self.n)(*pcm)
        out = ctypes.create_string_buffer(self.nb)
        self.lib.codec2_encode(self.enc, out, buf)
        return out.raw

    def descodifica(self, bits):      # 6 bytes -> 320 int16
        buf = (ctypes.c_short * self.n)()
        self.lib.codec2_decode(self.dec, buf, bits)
        return list(buf)


def clip(v):
    return -32768 if v < -32768 else 32767 if v > 32767 else int(v)


# ----------------------------------------------- protocolo cliente 4460 --
FEND, FESC, TFEND, TFESC = 0xC0, 0xDB, 0xDC, 0xDD
CMD_INICIO, CMD_VOZ, CMD_FIN, CMD_ESTADO, CMD_IDENT = 0x01, 0x02, 0x03, 0x05, 0x0E
CMD_ORIGEN = 0x1B                     # [texto]  quien habla de verdad (informativo)
EV_INICIO, EV_VOZ, EV_FIN, EV_ESTADO, EV_PTT, EV_LOG = 0x81, 0x82, 0x83, 0x85, 0x89, 0x8F
MODO_1200, TRAMAS_LOTE = 4, 12        # 12 x 40 ms = lote de 480 ms, como la app


def enmarcar(tipo, datos=b''):
    out = bytearray([FEND, tipo])
    for b in datos:
        out += bytes([FESC, TFEND]) if b == FEND else bytes([FESC, TFESC]) if b == FESC else bytes([b])
    out.append(FEND)
    return bytes(out)


class Desentrama:
    def __init__(self):
        self.buf, self.dentro, self.esc = bytearray(), False, False

    def mete(self, datos):
        for c in datos:
            if c == FEND:
                if self.dentro and self.buf:
                    yield self.buf[0], bytes(self.buf[1:])
                self.dentro, self.esc, self.buf = True, False, bytearray()
            elif self.dentro:
                if c == FESC:
                    self.esc = True
                    continue
                if self.esc:
                    c = FEND if c == TFEND else FESC
                    self.esc = False
                self.buf.append(c)


# ----------------------------------------------------------------- origen --
class VigiaOrigen:
    """Sigue el log del MMDVM_Bridge para saber quien habla en la red DMR.

    AllStar solo pasa audio: el indicativo se pierde en el enlace ASL. Pero el
    MMDVM_Bridge de la misma maquina apunta cada cabecera de voz que le llega de
    la red, y su cabecera precede en ~0,5 s al keyup del 1934. Se da por bueno si
    la pasada de ADN sigue abierta o acaba de empezar."""
    CAB = re.compile(r'(\d\d:\d\d:\d\d\.\d+) DMR Slot \d, received network voice header from (\S+) to (?:TG )?(\S+)')
    FIN = re.compile(r'received network end of voice|network watchdog')

    def __init__(self):
        self.quien, self.t, self.abierta = None, 0.0, False

    def bucle(self):
        f, ruta = None, None
        while True:
            r = time.strftime(ORIGEN_LOG)
            if r != ruta and os.path.exists(r):
                primero = f is None
                if f:
                    f.close()
                f, ruta = open(r, errors='replace'), r
                if primero:
                    f.seek(0, 2)          # al arrancar, lo viejo no interesa;
                                          # el fichero del dia nuevo, entero
            if not f:
                time.sleep(2)
                continue
            linea = f.readline()
            if not linea:
                time.sleep(0.1)
                continue
            m = self.CAB.search(linea)
            if m:
                self.quien, self.t, self.abierta = m.group(2), time.time(), True
            elif self.FIN.search(linea):
                self.abierta = False

    def ahora(self):
        """Indicativo de quien habla en la red, o None."""
        if self.quien and (time.time() - self.t < 3 or (self.abierta and time.time() - self.t < 120)):
            return self.quien
        return None


# ------------------------------------------------------------------- usrp --
HDR = struct.Struct('>4sIIIIIII')     # USRP, seq, memory, keyup, tg, type, mpxid, reserved


class Puente:
    def __init__(self):
        self.c2 = Codec2()
        self.usrp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.usrp.bind(USRP_ESCUCHA)
        self.seq = 0
        self.tcp = None
        self.tcp_lock = threading.Lock()
        self.ult_rx_tcp = 0.0
        # ASL -> LoRa
        self.tx = False               # transmitiendo hacia LoRa
        self.tx_t0 = 0.0
        self.tx_bloqueado = False     # TOT o turno perdido: hasta que ASL suelte
        self.ult_usrp = 0.0
        self.pcm_tx = []
        self.lote = []
        # LoRa -> ASL
        self.rx_cola = queue.Queue()
        self.rx_activo = False
        self.rx_fin = False
        self.ult_rx_voz = 0.0
        # La misma pasada llega por varios caminos (celdas, reflector) con el
        # mismo (src, stream, seq): sin esto cada copia se oia en AllStar -> eco.
        self.rx_clave = None          # (src×3, stream) de la pasada en curso
        self.rx_ult_seq = -1
        self.rx_dup = 0
        self.rx_acabada = (None, 0.0)  # (clave, cuando) de la ultima pasada
        self.vigia = VigiaOrigen()

    # ---------------------------------------------------------- servidor --
    def manda(self, tipo, datos=b''):
        with self.tcp_lock:
            s = self.tcp
            if s is None:
                return False
            try:
                s.sendall(enmarcar(tipo, datos))
                return True
            except OSError:
                return False

    def bucle_servidor(self):
        while True:
            try:
                s = socket.create_connection(SERVIDOR, 10)
                s.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
                s.settimeout(1.0)
                with self.tcp_lock:
                    self.tcp = s
                self.ult_rx_tcp = time.time()
                self.manda(CMD_IDENT, INDICATIVO.encode('ascii'))
                log('[lora] conectado a %s:%d como %s' % (SERVIDOR + (INDICATIVO,)))
                d, ult_latido = Desentrama(), 0.0
                while True:
                    ahora = time.time()
                    if ahora - ult_latido > 15:
                        self.manda(CMD_ESTADO)
                        ult_latido = ahora
                    if ahora - self.ult_rx_tcp > 45:
                        raise OSError('servidor mudo 45 s')
                    try:
                        trozo = s.recv(4096)
                    except socket.timeout:
                        continue
                    if not trozo:
                        raise OSError('cerrado por el servidor')
                    self.ult_rx_tcp = time.time()
                    for tipo, carga in d.mete(trozo):
                        self.evento(tipo, carga)
            except OSError as e:
                log('[lora] caido (%s), reintento en 5 s' % e)
            with self.tcp_lock:
                self.tcp = None
            self.tx = False
            time.sleep(5)

    def evento(self, tipo, carga):
        if tipo == EV_INICIO and len(carga) >= 6:
            ind = carga[7:].split(b'\0')[0].decode('ascii', 'replace').strip() if len(carga) > 7 else '?'
            if self.tx:
                log('[lora] %s empieza mientras transmitimos: se ignora' % ind)
                return
            clave = carga[2:6]
            if clave == self.rx_acabada[0] and time.time() - self.rx_acabada[1] < 10:
                return                    # copia rezagada de una pasada ya oida
            if self.rx_activo and self.rx_clave is not None:
                if clave != self.rx_clave:
                    log('[lora] %s empieza mientras suena otra pasada: se ignora' % ind)
                return                    # copia del mismo INICIO
            log('[lora->asl] habla %s' % ind)
            self.rx_clave, self.rx_ult_seq, self.rx_dup = clave, -1, 0
            self.rx_activo, self.rx_fin = True, False
            self.ult_rx_voz = time.time()
        elif tipo == EV_VOZ and len(carga) >= 9:
            if self.tx:
                return
            cuerpo = carga[7:]
            if cuerpo[0] != MODO_1200:
                return
            n = cuerpo[1]
            if len(cuerpo) < 2 + n * 6:
                return
            clave, sq = carga[2:6], carga[6]
            if clave == self.rx_acabada[0] and time.time() - self.rx_acabada[1] < 10:
                return
            if not self.rx_activo or self.rx_clave is None:
                self.rx_clave, self.rx_ult_seq, self.rx_dup = clave, -1, 0
                self.rx_activo, self.rx_fin = True, False
            elif clave != self.rx_clave:
                return                    # otra pasada a la vez: no se mezcla
            if sq <= self.rx_ult_seq:
                self.rx_dup += 1          # copia por otro camino
                return
            self.rx_ult_seq = sq
            for i in range(n):
                pcm = self.c2.descodifica(cuerpo[2 + i * 6: 8 + i * 6])
                if GANANCIA_A_ASL != 1.0:
                    pcm = [clip(v * GANANCIA_A_ASL) for v in pcm]
                self.rx_cola.put(pcm)
            self.ult_rx_voz = time.time()
        elif tipo == EV_FIN:
            if self.rx_activo and (self.rx_clave is None or carga[:4] == self.rx_clave):
                self.rx_fin = True
        elif tipo == EV_PTT and carga[:1] == b'\x02':
            if self.tx:
                log('[asl->lora] la red da el turno a %s: cortamos' % carga[1:].decode('ascii', 'replace'))
                self.fin_tx(bloquear=True)
        elif tipo == EV_LOG:
            log('[lora] aviso del servidor: %s' % carga.decode('utf-8', 'replace'))

    # --------------------------------------------------------- asl -> lora --
    def bucle_usrp(self):
        while True:
            d, _ = self.usrp.recvfrom(2048)
            if len(d) < 32 or d[:4] != b'USRP':
                continue
            keyup = HDR.unpack_from(d)[3]
            if not keyup:
                if self.tx:
                    self.fin_tx()
                self.tx_bloqueado = False
                continue
            self.ult_usrp = time.time()
            if self.tx_bloqueado:
                continue
            if self.rx_activo:              # semiduplex: LoRa tiene el canal
                continue
            if not self.tx:
                if not self.manda(CMD_INICIO, bytes([MODO_1200])):
                    continue
                self.tx, self.tx_t0, self.pcm_tx, self.lote = True, time.time(), [], []
                quien = self.vigia.ahora() if ORIGEN_LOG else None
                if quien:
                    self.manda(CMD_ORIGEN, ('%s (%s)' % (quien, ORIGEN_RED)).encode('ascii', 'ignore')[:20])
                log('[asl->lora] empieza%s' % (' (%s por %s)' % (quien, ORIGEN_RED) if quien else ''))
            if time.time() - self.tx_t0 > TOT_S:
                log('[asl->lora] TOT %.0f s: corte hasta que AllStar suelte' % TOT_S)
                self.fin_tx(bloquear=True)
                continue
            audio = d[32:]
            self.pcm_tx.extend(struct.unpack('<%dh' % (len(audio) // 2), audio))
            while len(self.pcm_tx) >= 320:
                trama, self.pcm_tx = self.pcm_tx[:320], self.pcm_tx[320:]
                if GANANCIA_A_LORA != 1.0:
                    trama = [clip(v * GANANCIA_A_LORA) for v in trama]
                self.lote.append(self.c2.codifica(trama))
                if len(self.lote) >= TRAMAS_LOTE:
                    self.manda_lote()

    def manda_lote(self):
        if self.lote:
            self.manda(CMD_VOZ, bytes([MODO_1200, len(self.lote)]) + b''.join(self.lote))
            self.lote = []

    def fin_tx(self, bloquear=False):
        if not self.tx:
            return
        self.manda_lote()
        self.manda(CMD_FIN)
        log('[asl->lora] fin (%.1f s)' % (time.time() - self.tx_t0))
        self.tx, self.pcm_tx = False, []
        self.tx_bloqueado = bloquear

    # --------------------------------------------------------- lora -> asl --
    def manda_usrp(self, keyup, pcm=None):
        self.seq = (self.seq + 1) & 0xFFFFFFFF
        audio = struct.pack('<160h', *pcm) if pcm else b''
        try:
            self.usrp.sendto(HDR.pack(b'USRP', self.seq, 0, 1 if keyup else 0, 0, 0, 0, 0) + audio, USRP_DESTINO)
        except OSError:
            pass

    def bucle_reproduce(self):
        """Saca a AllStar a ritmo de reloj (20 ms) lo que llega a lotes de 480 ms."""
        COLCHON = 0.6
        while True:
            # esperar a que empiece una pasada y llenar colchon
            if not self.rx_activo:
                time.sleep(0.02)
                continue
            t0 = time.time()
            while self.rx_cola.qsize() * 0.04 < COLCHON and not self.rx_fin and time.time() - t0 < COLCHON + 0.5:
                time.sleep(0.02)
            muestras, siguiente = [], time.monotonic()
            while True:
                if len(muestras) < 160:
                    try:
                        muestras.extend(self.rx_cola.get_nowait())
                    except queue.Empty:
                        if self.rx_fin or time.time() - self.ult_rx_voz > 2.0:
                            break
                        muestras.extend([0] * 160)        # hueco: silencio
                bloque, muestras = muestras[:160], muestras[160:]
                self.manda_usrp(True, bloque)
                siguiente += 0.02
                espera = siguiente - time.monotonic()
                if espera > 0:
                    time.sleep(espera)
            for _ in range(3):
                self.manda_usrp(False)
            log('[lora->asl] fin (%d copias descartadas)' % self.rx_dup)
            self.rx_acabada = (self.rx_clave, time.time())
            self.rx_clave = None
            while not self.rx_cola.empty():
                self.rx_cola.get_nowait()
            self.rx_activo, self.rx_fin = False, False

    def vigilante(self):
        """AllStar no siempre manda keyup=0: si deja de llegar audio, fin."""
        while True:
            time.sleep(0.1)
            if self.tx and time.time() - self.ult_usrp > 0.4:
                self.fin_tx()
            if self.tx_bloqueado and time.time() - self.ult_usrp > 1.0:
                self.tx_bloqueado = False

    def arranca(self):
        log('pttlora-asl: USRP %s:%d <-> %s:%d, servidor %s:%d, TOT %.0f s'
            % (USRP_ESCUCHA + USRP_DESTINO + SERVIDOR + (TOT_S,)))
        hilos = [self.bucle_servidor, self.bucle_usrp, self.bucle_reproduce, self.vigilante]
        if ORIGEN_LOG:
            hilos.append(self.vigia.bucle)
        for f in hilos:
            threading.Thread(target=f, daemon=True).start()
        while True:
            time.sleep(3600)


if __name__ == '__main__':
    Puente().arranca()
