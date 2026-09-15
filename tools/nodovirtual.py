#!/usr/bin/env python3
# nodovirtual.py — un "nodo" de PTT LoRa sin radio, hecho de software.
#
#   ./nodovirtual.py --a 192.168.4.1            se cuelga del enlace de un nodo
#   ./nodovirtual.py --escucha 4461             hace de punto de reunion
#   ./nodovirtual.py --escucha 4461 --enlace or.adan.ovh:4461 [--enlace-modo recibir]
#                    [--identidad /var/lib/pttlora/identidad --indicativo EA1ABC]
#                                               ...y ademas enlazado a otro
#   ./nodovirtual.py --escucha 4461 --politica politica.json --estado estado.json
#                                               ...con la guardia puesta
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
#
# EL ENLACE A OTRO REFLECTOR (--enlace). Una red propia —la de un grupo, con su
# reflector— no tiene por que ser una isla: con `--enlace` el reflector abre
# una conexion SALIENTE a otro (el principal) y la trata como a un par mas. Lo
# de sus celdas sale a la otra red y lo de la otra red entra en las suyas. Como
# la conexion la abre el de abajo, no hay que abrir puertos en ningun router.
#
#   --enlace-modo ambos     (defecto) se oye y se habla con la otra red
#   --enlace-modo recibir   solo se oye: lo de aqui no sale
#
# Un unico enlace por reflector, y a proposito: con uno, la red es un ARBOL y un
# arbol no tiene bucles. Aun asi alguien puede cerrar un circulo (A enlazado a
# B y B a A), y entre reflectores —que no descartan duplicados como los nodos—
# eso seria una tormenta. Por eso por el enlace no sube dos veces la misma
# trama, ni se acepta de vuelta una que ya cruzo (`Cruzadas`).
#
# LA GUARDIA (--politica / --estado). Abrir la red a reflectores ajenos es abrirla
# a sistemas mal configurados, con ruido, averiados o con mala intencion, y casi
# todos con IP dinamica. Lo que hay:
#
#   * IDENTIDAD, no IP. Un reflector enlazado se presenta con una clave Ed25519
#     (`identidad.py`) y firma un reto. Se le bloquea por esa clave, y cambiar
#     de IP no le sirve de nada.
#   * LO NUEVO ESCUCHA, TRAS UN PERIODO DE PRUEBA. Quien monta su red tiene que
#     poder probar que todo va sin esperar a nadie: una identidad nueva HABLA
#     durante `gracia_horas` (72 por defecto) desde que se vio por primera vez
#     —reconectar no lo reinicia—, y en ese tiempo el sysop la aprueba. Si no,
#     pasa a OIR sin hablar. Asi, a quien se bloquea y vuelve con una clave
#     nueva, eso solo le sirve para escuchar — y escuchar no hace transmitir a
#     la antena de nadie. Y para que no se lo salte con el periodo de prueba,
#     NO hay prueba si desde su misma red (/24) o con su indicativo ya se
#     bloqueo a otro reflector, si ya hay otro en prueba desde su red, o si hay
#     demasiados en prueba a la vez (una avalancha de claves nuevas).
#   * LISTA NEGRA por identidad, por indicativo (lo que diga el INICIO o la
#     baliza, entre por donde entre, tambien por el nodo de datos), por src de
#     estacion y por IP o rango. Con fecha de caducidad o para siempre.
#   * SANCIONES SOLAS, sin que nadie mire, A LOS SERVIDORES AJENOS IDENTIFICADOS:
#     tormenta de tramas, basura, ruido (pulsaciones de menos de un segundo en
#     rafaga), acaparar el turno hasta el TOT, reconectar sin parar, o no dar
#     abasto a recibir. Primera vez 10 min, segunda 1 h, tercera 24 h. Mientras
#     dura, se escucha pero no se habla (o fuera del todo, si es de las que
#     ahogan al servidor).
#   * LOS USUARIOS DE SIEMPRE, COMO SIEMPRE. La app entra por el nodo de datos,
#     que es local y no pasa por aqui; y una celda que se conecta sin
#     identidad no se sanciona sola nunca: una celda con la WiFi floja
#     reconecta mucho, y detras de una misma IP puede haber varias. Si una
#     conexion anonima inunda, se corta ESA conexion y ya; puede volver en el
#     acto. Para lo demas estan los bloqueos a mano.
#   * Y una conexion lenta ya no para a las demas: cada una tiene su cola.
#
# Lo gobierna `admin.py` (`sudo pttlora-admin`), que escribe la politica; el
# reflector la relee sola en dos segundos, sin reiniciar.

import ipaddress
import json
import os
import socket
import struct
import sys
import threading
import time
from collections import deque

FEND, FESC, TFEND, TFESC = 0xC0, 0xDB, 0xDC, 0xDD
CMD_AIRE = 0x11
# Control del enlace entre reflectores. Tipos KISS que el firmware no conoce y
# se salta (`del_enlace` solo mira CMD_AIRE), y que solo se mandan a quien se ha
# presentado con CMD_IDENT: una celda no los ve nunca.
CMD_IDENT, CMD_RETO, CMD_FIRMA, CMD_AVISO = 0x30, 0x31, 0x32, 0x33
CONTEXTO_FIRMA = b'pttlora-enlace-v1:'
PROTO_MAGIC = 0xA1
CAB_LEN = 8
T_VOZ, T_INICIO, T_FIN, T_HOLA, T_INFORME = 1, 2, 3, 4, 5
NOMBRES = {1: 'VOZ', 2: 'INICIO', 3: 'FIN', 4: 'HOLA', 5: 'INFORME'}


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
            if len(self.buf) < 2048:       # una trama infinita no llena la RAM
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
        cola = ' ' + t[CAB_LEN + 1:].split(b'\0')[0].decode('ascii', 'replace')
    elif tipo == T_HOLA and len(t) > CAB_LEN + 2:
        cola = ' ' + t[CAB_LEN + 2:].split(b'\0')[0].decode('ascii', 'replace')
    return '%-6s src=%06x stream=%d seq=%d saltos=%d%s' % (
        NOMBRES.get(tipo, '?%d' % tipo), src, t[6], t[7], saltos, cola)


def indicativo_de(t):
    """El indicativo que declara una trama (INICIO o baliza), o None."""
    tipo = t[2] >> 4
    if tipo == T_INICIO:
        cuerpo = t[CAB_LEN + 1:]
    elif tipo == T_HOLA:
        cuerpo = t[CAB_LEN + 2:]
    else:
        return None
    return cuerpo.split(b'\0')[0].decode('ascii', 'replace').strip().upper() or None


def texto_kv(d):
    """`clave=valor` separado por espacios, con `motivo` siempre el ultimo
    porque es el unico que lleva espacios dentro."""
    partes = ['%s=%s' % (k, v) for k, v in d.items() if k != 'motivo' and v != '']
    if d.get('motivo'):
        partes.append('motivo=%s' % d['motivo'])
    return ' '.join(partes).encode('utf-8', 'replace')


def lee_kv(b):
    s = b.decode('utf-8', 'replace')
    d = {}
    i = s.find('motivo=')
    if i == 0 or (i > 0 and s[i - 1] == ' '):
        d['motivo'] = s[i + 7:]
        s = s[:i]
    for p in s.split():
        if '=' in p:
            k, v = p.split('=', 1)
            d[k] = v
    return d


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

# ---------------------------------------------------------------- guardia ---
# Umbrales de las sanciones solas. La voz va en lotes de 480 ms: una
# transmision son ~2 tramas por segundo, y un enlace con varias celdas que se
# oyen entre si y sus balizas no pasa de unas decenas. 50 por segundo sostenidas
# no las hace nadie hablando.
TORMENTA_N, TORMENTA_S = 150, 3.0         # tramas en la ventana
BASURA_N, BASURA_S = 30, 10.0             # tramas que no son del protocolo
CORTAS_N, CORTAS_S, CORTA_DUR = 8, 60.0, 1.0   # pulsaciones de <1 s en 60 s
TOT_N, TOT_VENTANA = 2, 900.0             # turnos llevados al TOT en 15 min
RECONEXION_N, RECONEXION_S = 15, 600.0    # conexiones de un mismo sujeto
COLA_MAX = 400                            # tramas esperando a salir
ESCALA = [600, 3600, 86400]               # 10 min, 1 h, 24 h
MEMORIA_SANCION_S = 86400                 # que cuenta como "reincidente"
VERIFICA_S = 15.0                         # para firmar el reto
GRACIA_MAX = 10                           # reflectores en prueba a la vez
LOCALES = ('127.0.0.1', '::1')
POLITICA_DEFECTO = {
    # enlaces que se presentan con identidad y no estan aprobados
    'nuevos': 'escuchan',      # hablan | escuchan | fuera
    'gracia_horas': 72,        # periodo de prueba de una identidad nueva (0 = no)
    # conexiones que no se presentan: celdas con firmware, reflectores viejos
    'anonimas': 'hablan',      # hablan | escuchan
    'contacto': '',
    'aprobadas': {},           # clave hex -> {nota, cuando}
    'aprobadas_src': {},       # src hex -> {nota, cuando}
    'bloqueos': [],            # {que: id|ind|src|ip, valor, hasta, motivo, cuando}
    'perdones': {},            # sujeto -> cuando
}


def base_ind(ind):
    return ind.split('-')[0].split('/')[0] if ind else ''


def red_de(ip):
    """La red de una IP a efectos de "desde el mismo sitio": /24 o /48. Una IP
    dinamica cambia, pero casi siempre dentro del mismo rango de su operador."""
    try:
        a = ipaddress.ip_address(ip)
        return str(ipaddress.ip_network('%s/%d' % (ip, 24 if a.version == 4 else 48),
                                        strict=False))
    except ValueError:
        return ip


def fecha(t):
    return time.strftime('%d-%b %H:%M', time.localtime(t)) if t else 'para siempre'


class Cruzadas:
    """Tramas que han cruzado el enlace hace poco, en cualquier sentido. Los
    reflectores no tocan las tramas, asi que la trama entera es la llave.

    NO se aplica a lo que entra por los pares normales: dos celdas que oyen la
    misma transmision la mandan identica, y esas copias son las que cuenta el
    censo como `testigos`. Solo el enlace necesita el cortafuegos."""
    VIDA_S = 10.0

    def __init__(self):
        self.vistas = {}

    def ya(self, t):
        """True si ya cruzo; si no, la apunta."""
        ahora = time.time()
        if len(self.vistas) > 4096:
            self.vistas = {k: v for k, v in self.vistas.items()
                           if ahora - v < self.VIDA_S}
        v = self.vistas.get(t)
        if v is not None and ahora - v < self.VIDA_S:
            return True
        self.vistas[t] = ahora
        return False


class Conexion:
    """Un par. Lo que se le manda va a SU cola y lo saca SU hilo: un cliente
    con la linea atascada ya no puede dejar al reflector entero esperando en un
    `sendall` (antes pasaba, con el cerrojo global cogido)."""

    def __init__(self, sock, ip, puerto, saliente=None, locales=LOCALES):
        self.s = sock
        self.ip = ip
        self.saliente = bool(saliente)
        self.quien = ('ENLACE %s' % saliente) if saliente else '%s:%d' % (ip, puerto)
        self.local = (ip in locales) and not self.saliente
        self.clase = 'local' if self.local else 'anonima'   # verificando | enlace
        self.clave = None
        self.clave_propuesta = None
        self.ind = ''
        self.nombre = ''
        self.reto = None
        self.t_reto = 0.0
        self.src_aprobado = False
        self.desde = time.time()
        self.veredicto = None
        self.cola = deque()
        self.cv = threading.Condition()
        self.cerrada = False
        self.saliendo = False      # echada: se vacia la cola y se corta
        self.tramas = deque()
        self.basura = deque()
        self.cortas = deque()
        self.tots = deque()
        self.streams = {}
        self.n_tramas = 0
        self.n_pasadas = 0
        threading.Thread(target=self._escritor, daemon=True).start()

    def sujeto(self):
        return ('id:' + self.clave.hex()) if self.clave else ('ip:' + self.ip)

    def encola(self, datos, aunque_salga=False):
        with self.cv:
            if self.cerrada or (self.saliendo and not aunque_salga):
                return True
            if len(self.cola) >= COLA_MAX:
                return False
            self.cola.append(datos)
            self.cv.notify()
        return True

    def manda(self, cmd, datos):
        return self.encola(enmarcar(cmd, datos), aunque_salga=True)

    def _escritor(self):
        try:
            self.s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDTIMEO,
                              struct.pack('ll', 15, 0))
        except (OSError, struct.error):
            pass
        while True:
            with self.cv:
                while not self.cola and not self.cerrada and not self.saliendo:
                    self.cv.wait()
                if self.cerrada:
                    return
                if not self.cola:            # saliendo, y ya no queda nada
                    break
                datos = self.cola.popleft()
            try:
                self.s.sendall(datos)
            except OSError:
                break
        self.cierra()

    def cierra(self, despues=False):
        """`despues`: que el escritor deje salir antes lo que ya esta en la cola
        (el AVISO de por que se le echa) y corte el. Lo hace EL, y no quien
        llama, porque si no se corta el enchufe con el aviso a medio salir.
        Por si la otra punta no lee, a los 3 s se corta igual."""
        with self.cv:
            if self.cerrada:
                return
            if despues:
                if not self.saliendo:
                    self.saliendo = True
                    self.cv.notify_all()
                    t = threading.Timer(3.0, self.cierra)
                    t.daemon = True
                    t.start()
                return
            self.cerrada = True
            self.cola.clear()
            self.cv.notify_all()
        try:
            self.s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self.s.close()
        except OSError:
            pass


class Reflector:
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

    def __init__(self, puerto, enlace=None, modo='ambos', politica=None,
                 estado=None, identidad=None, indicativo='', nombre='',
                 locales=LOCALES):
        self.puerto = puerto
        self.enlace = enlace
        self.modo = modo
        self.f_politica = politica
        self.f_estado = estado
        self.indicativo = indicativo.upper()
        self.nombre = nombre
        self.locales = tuple(locales)
        self.secreto = self.clave = None
        import identidad as ident
        self.ident = ident
        if identidad:
            self.secreto, self.clave = ident.carga_o_crea(identidad)
            print('-- identidad de este reflector: ID %s' % ident.id_corto(self.clave))
        self.pares = []
        self.lock = threading.RLock()
        self.cruzadas = Cruzadas()
        self.turno = None      # [src, stream, empezo, ultima, conexion]
        self.descartadas = 0
        self.politica = dict(POLITICA_DEFECTO)
        self.mtime_politica = None
        self.vistos = {}       # clave hex -> {ind, nombre, ip, primera, ultima}
        self.sanciones = {}    # sujeto -> {veces: [t], vigente: {...}}
        self.reconexiones = {}  # sujeto -> deque de t
        self.src_ind = {}      # src -> (indicativo, cuando)
        self.estado_enlace = {}
        self.carga_estado()
        self.carga_politica()

    # ------------------------------------------------------- persistencia --
    def carga_estado(self):
        if not self.f_estado:
            return
        try:
            with open(self.f_estado) as f:
                e = json.load(f)
            self.vistos = e.get('vistos', {})
            self.sanciones = e.get('sanciones', {})
        except (OSError, ValueError):
            pass

    def guarda_estado(self):
        if not self.f_estado:
            return
        with self.lock:
            con = [{
                'quien': c.quien, 'ip': c.ip, 'clase': c.clase,
                'id': c.clave.hex() if c.clave else '', 'ind': c.ind,
                'nombre': c.nombre, 'desde': c.desde,
                'veredicto': (c.veredicto or ('', ''))[0],
                'motivo': (c.veredicto or ('', ''))[1],
                'tramas': c.n_tramas, 'pasadas': c.n_pasadas,
            } for c, _, _ in self.pares]
            e = {'cuando': time.time(), 'puerto': self.puerto,
                 'conexiones': con, 'vistos': self.vistos,
                 'sanciones': self.sanciones, 'descartadas': self.descartadas,
                 'enlace': dict(self.estado_enlace,
                                id=self.clave.hex() if self.clave else '',
                                a=self.enlace or '', modo=self.modo)}
            texto = json.dumps(e, indent=1)
        tmp = self.f_estado + '.tmp'
        try:
            with open(tmp, 'w') as f:
                f.write(texto)
            os.replace(tmp, self.f_estado)
        except OSError as ex:
            print('-- no puedo escribir %s: %s' % (self.f_estado, ex))

    def carga_politica(self):
        if not self.f_politica:
            return False
        try:
            m = os.stat(self.f_politica).st_mtime
        except OSError:
            return False
        if m == self.mtime_politica:
            return False
        try:
            with open(self.f_politica) as f:
                p = json.load(f)
            if not isinstance(p, dict):
                raise ValueError('no es un objeto')
        except (OSError, ValueError) as ex:
            print('-- politica ilegible (%s): se sigue con la anterior' % ex)
            self.mtime_politica = m
            return False
        nueva = dict(POLITICA_DEFECTO)
        nueva.update(p)
        with self.lock:
            self.politica = nueva
        self.mtime_politica = m
        print('-- politica cargada: nuevos %s, anonimas %s, %d bloqueos, '
              '%d aprobadas' % (nueva['nuevos'], nueva['anonimas'],
                                len(nueva['bloqueos']), len(nueva['aprobadas'])))
        return True

    # ------------------------------------------------------------ juicio --
    def bloqueo(self, que, valor):
        """El bloqueo vigente que aplica, o None."""
        if not valor:
            return None
        ahora = time.time()
        for b in self.politica.get('bloqueos', []):
            if b.get('hasta') and b['hasta'] < ahora:
                continue
            if b.get('que') != que:
                continue
            v = str(b.get('valor', ''))
            if que == 'ip':
                try:
                    if ipaddress.ip_address(valor) in ipaddress.ip_network(v, strict=False):
                        return b
                except ValueError:
                    pass
            elif que == 'ind':
                v = v.upper()
                if valor == v or ('-' not in v and '/' not in v and base_ind(valor) == v):
                    return b
            elif que == 'id':
                if len(v) >= 6 and valor.startswith(v.lower()):
                    return b
            elif valor == v.lower():
                return b
        return None

    def sancion_vigente(self, sujeto):
        h = self.sanciones.get(sujeto)
        s = h and h.get('vigente')
        if not s:
            return None
        perdon = self.politica.get('perdones', {}).get(sujeto, 0)
        if s['hasta'] < time.time() or perdon > s['cuando']:
            h['vigente'] = None
            return None
        return s

    def juzga(self, c):
        """('habla' | 'escucha' | 'fuera', motivo) para una conexion."""
        if c.local or c.saliente:
            return ('habla', '')
        b = self.bloqueo('ip', c.ip)
        if b:
            return ('fuera', 'bloqueado: %s' % (b.get('motivo') or 'IP'))
        if c.clave:
            b = self.bloqueo('id', c.clave.hex())
            if b:
                return ('fuera', 'bloqueado: %s' % (b.get('motivo') or 'identidad'))
        if c.clase == 'enlace' and c.ind:
            b = self.bloqueo('ind', c.ind)
            if b:
                return ('fuera', 'bloqueado: %s' % (b.get('motivo') or c.ind))
        if c.clave:
            s = self.sancion_vigente(c.sujeto())
            if s:
                return ('fuera' if s.get('grave') else 'escucha',
                        'sancion automatica por %s hasta %s'
                        % (s['motivo'], fecha(s['hasta'])))
        if c.clase == 'verificando':
            return ('escucha', 'comprobando la identidad')
        if c.clase == 'enlace':
            if c.clave.hex() in self.politica.get('aprobadas', {}):
                return ('habla', 'aprobada')
            n = self.politica.get('nuevos', 'escuchan')
            if n == 'fuera':
                return ('fuera', 'esta red solo admite enlaces aprobados')
            if n == 'hablan':
                return ('habla', 'sin aprobar, pero aqui los nuevos hablan')
            hasta, porque_no = self.gracia(c)
            if hasta:
                return ('habla', 'periodo de prueba hasta %s: pide que te '
                                 'aprueben antes de que acabe' % fecha(hasta))
            return ('escucha', 'pendiente de aprobacion: escuchas la red, pero lo '
                               'tuyo no sale hasta que te aprueben (%s)' % porque_no)
        if self.politica.get('anonimas', 'hablan') == 'escuchan' and not c.src_aprobado:
            return ('escucha', 'sin identificar')
        return ('habla', '')

    def gracia(self, c):
        """(hasta, '') si esta identidad esta en su periodo de prueba, o
        (0, por que no)."""
        try:
            horas = float(self.politica.get('gracia_horas', 72))
        except (TypeError, ValueError):
            horas = 0
        if horas <= 0:
            return 0, 'aqui no hay periodo de prueba'
        hx = c.clave.hex()
        ahora = time.time()
        hasta = self.vistos.get(hx, {}).get('primera', ahora) + horas * 3600
        if ahora > hasta:
            return 0, 'el periodo de prueba ya se acabo'
        red = red_de(c.ip)
        for b in self.politica.get('bloqueos', []):
            if b.get('que') != 'id' or (b.get('hasta') and b['hasta'] < ahora):
                continue
            for k, v in self.vistos.items():
                if k == hx or not k.startswith(str(b.get('valor', '-')).lower()):
                    continue
                if red_de(v.get('ip', '')) == red:
                    return 0, 'desde tu red ya se bloqueo a otro reflector'
                if c.ind and base_ind(v.get('ind', '')) == base_ind(c.ind):
                    return 0, 'ya se bloqueo a otro reflector de %s' % base_ind(c.ind)
        en_prueba = [o for o, _, _ in self.pares
                     if o is not c and o.clase == 'enlace' and o.veredicto
                     and o.veredicto[1].startswith('periodo de prueba')]
        if any(red_de(o.ip) == red for o in en_prueba):
            return 0, 'ya hay otro reflector en prueba desde tu red'
        if len(en_prueba) >= GRACIA_MAX:
            return 0, 'hay demasiados reflectores en prueba a la vez'
        return hasta, ''

    def aplica_juicio(self, c, avisa=True):
        with self.lock:
            v = self.juzga(c)
            cambia = v != c.veredicto
            c.veredicto = v
        if cambia:
            print('%s -> %s%s' % (c.quien, v[0], (' (%s)' % v[1]) if v[1] else ''))
            if avisa and c.clase == 'enlace':
                c.manda(CMD_AVISO, texto_kv({
                    'estado': v[0], 'hasta': int(self.hasta_de(c) or 0) or '',
                    'contacto': self.politica.get('contacto', '').replace(' ', '_'),
                    'motivo': v[1]}))
        if v[0] == 'fuera':
            c.cierra(despues=True)
        return v

    def hasta_de(self, c):
        for q, val in (('id', c.clave.hex() if c.clave else ''), ('ind', c.ind),
                       ('ip', c.ip)):
            b = self.bloqueo(q, val)
            if b:
                return b.get('hasta') or 0
        s = self.sancion_vigente(c.sujeto())
        return s['hasta'] if s else 0

    def sanciona(self, c, motivo, grave=False):
        """Castigo automatico con escalada, SOLO a reflectores identificados:
        va a su identidad, no a la conexion, asi que reconectar no lo borra.
        A una conexion anonima (una celda, o un reflector viejo) no se la
        castiga: si es grave se corta esa conexion y nada mas."""
        if not c.clave:
            if grave:
                print('!! %s: %s — se corta la conexion (sin sancion: no esta '
                      'identificada)' % (c.quien, motivo))
                c.cierra()
            return
        with self.lock:
            sj = c.sujeto()
            ahora = time.time()
            if self.sancion_vigente(sj):
                return
            h = self.sanciones.setdefault(sj, {'veces': [], 'vigente': None})
            h['veces'] = [t for t in h['veces'] if ahora - t < MEMORIA_SANCION_S] + [ahora]
            dur = ESCALA[min(len(h['veces']), len(ESCALA)) - 1]
            h['vigente'] = {'motivo': motivo, 'cuando': ahora, 'hasta': ahora + dur,
                            'grave': grave, 'ind': c.ind, 'ip': c.ip}
            afectadas = [o for o, _, _ in self.pares if o.sujeto() == sj]
        print('!! SANCION a %s (%s%s) por %s: %d min, %s' % (
            c.quien, sj[:11], (' ' + c.ind) if c.ind else '', motivo, dur // 60,
            'fuera' if grave else 'solo escucha'))
        for o in set(afectadas) | {c}:
            self.aplica_juicio(o)
        self.guarda_estado()

    def filtra_trama(self, t):
        """Motivo para no dejar pasar una trama por lo que ES, venga de donde
        venga (tambien del nodo de datos, que es local), o None."""
        src = (t[3] << 16) | (t[4] << 8) | t[5]
        sh = '%06x' % src
        if self.bloqueo('src', sh):
            return 'src %s bloqueado' % sh
        ind = indicativo_de(t)
        if ind:
            self.src_ind[src] = (ind, time.time())
            if len(self.src_ind) > 8192:
                viejo = time.time() - 3600
                self.src_ind = {k: v for k, v in self.src_ind.items() if v[1] > viejo}
        else:
            ind = self.src_ind.get(src, ('', 0))[0]
        if ind and self.bloqueo('ind', ind):
            return '%s bloqueado' % ind
        return None

    def vigila(self, c, t, tipo):
        """Las sanciones solas. Devuelve False si la conexion ha caido."""
        if c.local or c.saliente:
            return True
        ahora = time.time()
        c.tramas.append(ahora)
        while c.tramas and ahora - c.tramas[0] > TORMENTA_S:
            c.tramas.popleft()
        if len(c.tramas) > TORMENTA_N:
            c.tramas.clear()
            self.sanciona(c, 'tormenta de tramas', grave=True)
            return False
        if tipo == T_INICIO:
            k = (t[3:6], t[6])
            if k not in c.streams:
                c.streams[k] = ahora
            if len(c.streams) > 512:
                c.streams = {k: v for k, v in c.streams.items() if ahora - v < 300}
        elif tipo == T_FIN:
            t0 = c.streams.pop((t[3:6], t[6]), None)
            if t0 is not None and ahora - t0 < CORTA_DUR:
                c.cortas.append(ahora)
                while c.cortas and ahora - c.cortas[0] > CORTAS_S:
                    c.cortas.popleft()
                if len(c.cortas) > CORTAS_N:
                    c.cortas.clear()
                    self.sanciona(c, 'ruido (pulsaciones cortas en rafaga)')
        return True

    def basura(self, c):
        if c.local or c.saliente:
            return
        ahora = time.time()
        c.basura.append(ahora)
        while c.basura and ahora - c.basura[0] > BASURA_S:
            c.basura.popleft()
        if len(c.basura) > BASURA_N:
            c.basura.clear()
            self.sanciona(c, 'tramas que no son del protocolo')

    # ---------------------------------------------------------- arbitraje --
    def deja_pasar(self, t, c):
        """Arbitraje: True si esta trama debe repartirse."""
        tipo = t[2] >> 4
        src = (t[3] << 16) | (t[4] << 8) | t[5]
        stream = t[6]
        ahora = time.time()
        q = self.turno
        # Las balizas no se arbitran: son identificacion de estacion, no voz,
        # y callarlas seria justo lo contrario de lo que hay que hacer.
        # El INFORME tampoco: no es voz, no ocupa el canal de nadie (ni siquiera
        # sale al aire) y es lo que alimenta el censo. Ademas, si cayera en el
        # arbitraje del turno podria ROBARSELO a quien esta hablando.
        if tipo in (T_HOLA, T_INFORME):
            return True
        if q and ahora - q[2] > TOT_S:
            dueño = q[4]
            if dueño is not None and not dueño.local and not dueño.saliente:
                dueño.tots.append(ahora)
                while dueño.tots and ahora - dueño.tots[0] > TOT_VENTANA:
                    dueño.tots.popleft()
                if len(dueño.tots) >= TOT_N:
                    dueño.tots.clear()
                    self.sanciona(dueño, 'acaparar el turno hasta el TOT')
            q = self.turno = None
        if q and ahora - q[3] > MUDO_S:
            q = self.turno = None          # se fue sin despedirse
        if q is None:
            # Un FIN no coge el turno: es el final de algo, no el principio.
            # Si lo cogiera (pasa cuando el FIN de una transmision pisada llega
            # despues de que la buena termine), dejaria la red muda hasta que
            # caducara — y quien hablara a continuacion no saldria.
            if tipo == T_FIN:
                return True
            self.turno = [src, stream, ahora, ahora, c]
            return True
        if q[0] == src and q[1] == stream:
            q[3] = ahora
            if tipo == T_FIN:
                self.turno = None
            return True
        self.descartadas += 1
        return False

    # ------------------------------------------------------ una conexion --
    def control(self, c, cmd, p):
        """Tramas de control del enlace entre reflectores."""
        ahora = time.time()
        if c.saliente:
            if cmd == CMD_RETO and self.secreto and len(p) == 32:
                c.manda(CMD_FIRMA, self.ident.firma(self.secreto, CONTEXTO_FIRMA + p))
            elif cmd == CMD_AVISO:
                d = lee_kv(p)
                h = d.get('hasta', '')
                self.estado_enlace = {'estado': d.get('estado', ''),
                                      'motivo': d.get('motivo', ''),
                                      'hasta': int(h) if h.isdigit() else 0,
                                      'contacto': d.get('contacto', '').replace('_', ' '),
                                      'cuando': ahora}
                print('-- el reflector de arriba dice: %s%s%s' % (
                    d.get('estado', '?'),
                    (' — ' + d['motivo']) if d.get('motivo') else '',
                    (' · contacto: ' + self.estado_enlace['contacto'])
                    if self.estado_enlace['contacto'] else ''))
                if self.clave and d.get('estado') != 'habla':
                    print('-- ID de este reflector, para quien administra aquella '
                          'red: %s' % self.ident.id_corto(self.clave))
            return
        if cmd == CMD_IDENT and c.clase == 'anonima' and not c.local:
            d = lee_kv(p)
            try:
                clave = bytes.fromhex(d.get('clave', ''))
            except ValueError:
                clave = b''
            if len(clave) != 32:
                self.basura(c)
                return
            c.clave_propuesta = clave
            c.ind = d.get('ind', '')[:16].upper()
            c.nombre = d.get('nombre', '')[:32].replace('_', ' ')
            c.reto = os.urandom(32)
            c.t_reto = ahora
            c.clase = 'verificando'
            c.manda(CMD_RETO, c.reto)
            self.aplica_juicio(c, avisa=False)
        elif cmd == CMD_FIRMA and c.clase == 'verificando':
            if not self.ident.verifica(c.clave_propuesta, CONTEXTO_FIRMA + c.reto, p):
                print('%s: firma FALSA para la clave %s — fuera'
                      % (c.quien, c.clave_propuesta.hex()[:8]))
                c.cierra()
                return
            c.clave, c.clase, c.reto = c.clave_propuesta, 'enlace', None
            hx = c.clave.hex()
            with self.lock:
                viejas = [o for o, _, _ in self.pares if o is not c and o.clave == c.clave]
                v = self.vistos.setdefault(hx, {'primera': ahora})
                v.update({'ind': c.ind, 'nombre': c.nombre, 'ip': c.ip,
                          'ultima': ahora})
            for o in viejas:                    # la misma identidad dos veces
                print('%s: %s se ha vuelto a conectar; fuera la vieja' % (o.quien, hx[:8]))
                o.cierra()
            print('%s es el reflector %s (%s %s)' % (c.quien, hx[:8], c.ind, c.nombre))
            if not self.cuenta_reconexion(c):
                return
            c.veredicto = None
            self.aplica_juicio(c)
            self.guarda_estado()        # que salga ya en `pttlora-admin pendientes`
        else:
            self.basura(c)

    def cuenta_reconexion(self, c):
        if c.local or not c.clave:
            return True
        sj = c.sujeto()
        ahora = time.time()
        with self.lock:
            d = self.reconexiones.setdefault(sj, deque())
            d.append(ahora)
            while d and ahora - d[0] > RECONEXION_S:
                d.popleft()
            demasiadas = len(d) > RECONEXION_N
            if demasiadas:
                d.clear()
        if demasiadas:
            self.sanciona(c, 'reconectar sin parar', grave=True)
            return False
        return True

    def reparte(self, c, p):
        tipo = p[2] >> 4
        atascadas = []
        with self.lock:
            if c.saliente and self.cruzadas.ya(p):
                return                     # vuelve algo que ya subio: bucle
            # Una celda aprobada por src se "presenta" con su baliza. Se mira
            # ANTES que el veredicto: si no, a quien solo escucha se le tiraria
            # la baliza que le da permiso para hablar.
            aprobar = (tipo == T_HOLA and not c.src_aprobado and not c.local
                       and '%02x%02x%02x' % (p[3], p[4], p[5])
                       in self.politica.get('aprobadas_src', {}))
            if aprobar:
                c.src_aprobado = True
                c.veredicto = None
            elif (c.veredicto or ('habla',))[0] != 'habla':
                return
            motivo = None if aprobar else self.filtra_trama(p)
            if motivo:
                self.descartadas += 1
                print('  %s XX %s (%s)' % (c.quien, describe(p), motivo))
                return
            if not aprobar:
                pasa = self.deja_pasar(p, c)
                print('  %s %s %s' % (c.quien, '->' if pasa else 'XX', describe(p)))
                if not pasa:
                    return
                c.n_pasadas += 1
                marco = enmarcar(CMD_AIRE, p)
                for otro, _, es_enlace in self.pares:
                    if (otro is c or otro.clase == 'verificando' or otro.cerrada
                            or otro.saliendo):
                        continue
                    if es_enlace and (self.modo == 'recibir' or self.cruzadas.ya(p)):
                        continue
                    if not otro.encola(marco):
                        atascadas.append(otro)
        if aprobar:
            self.aplica_juicio(c)
            return self.reparte(c, p)
        for otro in atascadas:
            print('%s no da abasto a recibir: fuera' % otro.quien)
            if not otro.local and not otro.saliente:
                self.sanciona(otro, 'no dar abasto a recibir', grave=True)
            otro.cierra()

    def atiende(self, c):
        d = Desentrama()
        try:
            while not c.cerrada:
                b = c.s.recv(4096)
                if not b:
                    break
                for cmd, p in d.mete(b):
                    if c.cerrada:
                        break
                    if cmd != CMD_AIRE:
                        if cmd in (CMD_IDENT, CMD_RETO, CMD_FIRMA, CMD_AVISO):
                            self.control(c, cmd, p)
                        else:
                            self.basura(c)
                        continue
                    if len(p) < CAB_LEN or p[0] != PROTO_MAGIC:
                        self.basura(c)
                        continue
                    c.n_tramas += 1
                    if not self.vigila(c, p, p[2] >> 4):
                        break
                    self.reparte(c, p)
        except OSError:
            pass
        finally:
            with self.lock:
                self.pares[:] = [x for x in self.pares if x[0] is not c]
            c.cierra()
            print('%s se fue' % c.quien)

    # --------------------------------------------------------- los hilos --
    def sube(self):
        """Mantiene el enlace con el otro reflector: si se cae, se reintenta
        con espera creciente, y la red de aqui sigue funcionando entretanto."""
        host, _, p = self.enlace.rpartition(':')
        host = host.strip('[]')
        espera = 5
        while True:
            try:
                s = socket.create_connection((host, int(p)), 10)
                s.settimeout(None)
                s.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            except OSError as e:
                print('enlace %s: no conecta (%s); otra vez en %d s'
                      % (self.enlace, e, espera))
                self.estado_enlace = {'estado': 'sin conexion', 'motivo': str(e),
                                      'cuando': time.time()}
                time.sleep(espera)
                espera = min(espera * 2, 300)
                continue
            c = Conexion(s, host, int(p), saliente=self.enlace)
            c.veredicto = ('habla', '')
            print('%s enganchado (%s)' % (c.quien, 'se oye y se habla'
                                          if self.modo == 'ambos' else 'solo se oye'))
            self.estado_enlace = {'estado': 'conectado', 'cuando': time.time()}
            if self.clave:
                c.manda(CMD_IDENT, texto_kv({
                    'v': 1, 'clave': self.clave.hex(), 'ind': self.indicativo,
                    'nombre': self.nombre.replace(' ', '_'), 'modo': self.modo}))
            with self.lock:
                self.pares.append((c, c.quien, True))
            t0 = time.time()
            self.atiende(c)
            # Si arriba nos han echado, no se insiste cada pocos segundos:
            # hasta que acabe el castigo (y como mucho un dia), o una hora.
            ee = self.estado_enlace
            if ee.get('estado') == 'fuera':
                espera = int(max(3600, min(ee.get('hasta', 0) - time.time(), 86400)))
                print('-- nos han echado del enlace; se reintenta en %d min' % (espera // 60))
                time.sleep(espera)
                espera = 5
                continue
            if time.time() - t0 > 60:
                espera = 5
            time.sleep(espera)
            espera = min(espera * 2, 300)

    def mantenimiento(self):
        """Cada segundo mira si la politica ha cambiado y vuelve a juzgar a
        todos; corta las verificaciones que no llegan, y deja el estado en
        disco cada pocos segundos."""
        n = 0
        while True:
            time.sleep(1)
            n += 1
            self.latido = time.time()
            try:
                cambio = self.carga_politica()
                ahora = time.time()
                with self.lock:
                    pares = [c for c, _, _ in self.pares]
                for c in pares:
                    if c.clase == 'verificando' and ahora - c.t_reto > VERIFICA_S:
                        print('%s no firma el reto a tiempo: fuera' % c.quien)
                        c.cierra()
                        continue
                    if cambio or n % 5 == 0:
                        self.aplica_juicio(c)
                if n % 5 == 0 or cambio:
                    with self.lock:
                        for sj in list(self.sanciones):
                            self.sancion_vigente(sj)     # limpia caducadas y perdonadas
                            h = self.sanciones[sj]
                            h['veces'] = [t for t in h.get('veces', [])
                                          if ahora - t < MEMORIA_SANCION_S]
                            if not h['veces'] and not h.get('vigente'):
                                del self.sanciones[sj]
                    self.guarda_estado()
            except Exception as ex:          # el mantenimiento no puede morir
                print('-- mantenimiento: %r' % ex)

    def sirve(self):
        try:
            srv = socket.socket(socket.AF_INET6)
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
            srv.bind(('::', self.puerto))
        except OSError:
            srv = socket.socket()
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind(('', self.puerto))
        srv.listen(32)
        print('reflector escuchando en el puerto %d' % self.puerto)
        threading.Thread(target=self.mantenimiento, daemon=True).start()
        if self.enlace:
            threading.Thread(target=self.sube, daemon=True).start()
        while True:
            s, dir_ = srv.accept()
            ip = dir_[0]
            if ip.startswith('::ffff:'):
                ip = ip[7:]
            try:
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            except OSError:
                pass
            c = Conexion(s, ip, dir_[1], locales=self.locales)
            with self.lock:
                v = self.juzga(c)
            if v[0] == 'fuera':
                # Por IP o por sancion de su IP: ni se le escucha. Se le dice
                # por que (a una celda le da igual: no entiende el AVISO).
                print('%s rechazado: %s' % (c.quien, v[1]))
                c.manda(CMD_AVISO, texto_kv({'estado': 'fuera', 'motivo': v[1]}))
                c.cierra(despues=True)
                continue
            if not self.cuenta_reconexion(c):
                c.cierra()
                continue
            c.veredicto = v
            print('%s enganchado' % c.quien)
            with self.lock:
                self.pares.append((c, c.quien, False))
            threading.Thread(target=self.atiende, args=(c,), daemon=True).start()


def main():
    def opt(nombre, defecto=None):
        return (sys.argv[sys.argv.index(nombre) + 1]
                if nombre in sys.argv else defecto)

    vistas = []
    if '--escucha' in sys.argv:
        modo = opt('--enlace-modo', 'ambos')
        if modo not in ('ambos', 'recibir'):
            print('--enlace-modo es ambos o recibir')
            return 2
        # Sin buffer: si corre como servicio, lo que se escribe tiene que
        # aparecer en el registro cuando pasa, no cuando se llene un buffer.
        try:
            sys.stdout.reconfigure(line_buffering=True)
        except AttributeError:
            pass
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        # --locales: que direcciones son de esta maquina (nodo de datos, censo)
        # y no pasan por la guardia. Los bancos lo vacian para probarla.
        locales = [x for x in opt('--locales', ','.join(LOCALES)).split(',')
                   if x and x != 'ninguna']
        Reflector(int(opt('--escucha', '4461')), opt('--enlace'), modo,
                  opt('--politica'), opt('--estado'), opt('--identidad'),
                  opt('--indicativo', ''), opt('--nombre', ''), locales).sirve()
        return 0
    host = opt('--a')
    if not host:
        with open(__file__) as f:
            print(''.join(f.readlines()[1:12]))
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
