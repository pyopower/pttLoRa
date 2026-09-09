#!/usr/bin/env python3
"""monitor.py — puesto de escucha para las pruebas de campo.

    ./venv/bin/python tools/monitor.py [--tcp 192.168.1.50] [--celda 14481]

QUE HACE Y POR QUE
------------------
El usuario se va a andar con el movil y el nodo viajero. Aqui se queda esto
apuntando TODO lo que pasa, para que al volver haya datos y no impresiones:

  * Cada transmision que llega: quien, cuando, **con que RSSI y SNR**, cuantos
    lotes traia y **cuantos faltan** (los huecos de la numeracion). El audio se
    reconstruye a WAV, asi que ademas se puede escuchar como sonaba a esa
    distancia.
  * Cada baliza que se oye, con su RSSI: es la forma barata de saber si un nodo
    sigue al alcance aunque nadie hable.
  * **Los contadores de la CELDA, por el relevo.** Esto es lo que de verdad
    importa y no se ve desde casa: cuando el viajero se aleja, la pregunta no es
    "¿le oigo yo?" sino **"¿le sigue oyendo la celda del tejado?"**. Si la celda
    recibe y repite pero en casa ya no llega, el problema esta en la bajada, no
    en la subida — y eso son dos averias distintas que desde casa se ven igual.

Lo escribe todo en `bench/monitor-AAAAMMDD.log` con hora, y deja el resumen en
`bench/monitor-estado.txt` para poder mirarlo de un vistazo sin cortar nada.

NO TRANSMITE. A proposito: el nodo admite cuatro clientes por WiFi a la vez, asi
que para hablar se lanza `nodo.py --tcp <ip> hablar <wav>` en paralelo y este
sigue escuchando. Una cosa hace una cosa.
"""
import os
import subprocess
import sys
import time

AQUI = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, AQUI)
from nodo import Nodo, MODOS

POR_CODIGO = {v[0]: k for k, v in MODOS.items()}
CORTE_S = 5.0        # sin lotes durante esto = transmision terminada aunque no llegue el FIN
BENCH = os.path.join(os.path.dirname(AQUI), 'bench')


def ahora():
    return time.strftime('%H:%M:%S')


class Diario:
    def __init__(self):
        os.makedirs(BENCH, exist_ok=True)
        self.ruta = os.path.join(BENCH, 'monitor-%s.log' % time.strftime('%Y%m%d'))
        self.estado = os.path.join(BENCH, 'monitor-estado.txt')
        self.resumen = []

    def apunta(self, linea):
        txt = '%s  %s' % (ahora(), linea)
        print(txt, flush=True)
        with open(self.ruta, 'a', encoding='utf-8') as f:
            f.write(txt + '\n')
        self.resumen.append(txt)
        del self.resumen[:-40]
        with open(self.estado, 'w', encoding='utf-8') as f:
            f.write('\n'.join(self.resumen) + '\n')


def guarda_audio(d, diario, cortada=False):
    """Reconstruye la voz recibida a WAV. Poder ESCUCHAR como sonaba a 2 km
    vale mas que cualquier contador.

    `cortada` = se cerro por silencio, sin haber oido el FIN. La grabacion vale
    igual (es lo que llego), pero conviene que el diario lo diga."""
    nombre = POR_CODIGO.get(d['modo'], '1200')
    orden = sorted(d['lotes'])
    bits = b''.join(d['lotes'][k] for k in orden)
    huecos = (orden[-1] - orden[0] + 1 - len(orden)) if orden else 0
    wav = os.path.join(BENCH, 'rx-%s-%s.wav'
                       % (d['ind'].replace('/', '_') or 'anon',
                          time.strftime('%H%M%S')))
    try:
        open('/tmp/.mon.bin', 'wb').write(bits)
        subprocess.run(['c2dec', nombre, '/tmp/.mon.bin', '/tmp/.mon.raw'],
                       check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        subprocess.run(['sox', '-r', '8000', '-e', 'signed-integer', '-b', '16',
                        '-c', '1', '/tmp/.mon.raw', wav], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        segs = os.path.getsize('/tmp/.mon.raw') / 16000.0
        diario.apunta('%s %s: %d lotes, %d perdidos, %.1f s  rssi=%d snr=%d  -> %s'
                      % ('<< CORTE' if cortada else '<< FIN ',
                         d['ind'], len(orden), huecos, segs, d['rssi'], d['snr'],
                         os.path.basename(wav)))
    except Exception as e:
        diario.apunta('%s %s: %d lotes, %d perdidos (sin audio: %s)'
                      % ('<< CORTE' if cortada else '<< FIN ',
                         d['ind'], len(orden), huecos, e))


class Relevo:
    """Los contadores de la celda, por el tunel del relevo, con UNA SOLA
    CONEXION mantenida abierta.

    ⚠️ **LA RANURA DEL RELEVO SOLO ADMITE UN OPERADOR.** Antes esto abria y
    cerraba una conexion en cada sondeo, y cada sondeo echaba a quien estuviera
    dentro: el 8-sep-2026 tumbo tres veces seguidas la actualizacion de la celda
    del tejado, siempre por el 10%, y el sintoma (`BrokenPipeError`) no apuntaba
    a su causa ni de lejos — parecia la red del tejado flaqueando.

    Ahora se toma la ranura una vez y se conserva. Y, lo que de verdad importa:
    **si nos echan, no peleamos por volver.** Que nos tiren significa que otro
    ha entrado (una actualizacion, una consulta a mano), asi que se espera
    `ESPERA_ECHADO` antes de reintentar. Un monitor ciego un rato no rompe nada;
    un monitor terco rompe una OTA.

    Aun asi, para actualizar por el relevo sigue siendo mas limpio parar el
    monitor o arrancarlo con `--celda 0`.
    """

    ESPERA_ECHADO = 300.0                   # 5 min de tregua si nos desconectan

    def __init__(self, puerto, diario):
        self.puerto = puerto
        self.diario = diario
        self.c = None
        self.callado_hasta = 0.0
        self.avisado = False

    def _conecta(self):
        if self.c:
            return True
        if time.time() < self.callado_hasta:
            return False
        try:
            self.c = Nodo(tcp='127.0.0.1:%d' % self.puerto)
            if self.avisado:
                self.diario.apunta('   [celda] ranura del relevo recuperada')
                self.avisado = False
            return True
        except Exception as e:
            self.c = None
            self.callado_hasta = time.time() + 30.0
            # Se avisa, y esto NO es opcional: la primera version de esta clase
            # se callaba tambien los fallos de conexion, y el resultado fue una
            # hora entera sin un solo dato de la celda y sin una sola linea que
            # lo dijera. Un monitor que falla en silencio es peor que no tener
            # monitor, porque el silencio se lee como "no pasa nada".
            if not self.avisado:
                self.diario.apunta('   [celda] no se puede entrar al relevo (%s: %s)'
                                   % (type(e).__name__, e))
                self.avisado = True
            return False

    def _suelta(self, motivo):
        try:
            self.c.s.close()
        except Exception:
            pass
        self.c = None
        self.callado_hasta = time.time() + self.ESPERA_ECHADO
        if not self.avisado:
            self.diario.apunta('   [celda] fuera del relevo (%s): callado %d min '
                               'por si alguien lo esta usando'
                               % (motivo, int(self.ESPERA_ECHADO / 60)))
            self.avisado = True

    def contadores(self):
        """Devuelve dict de campos o None."""
        if not self._conecta():
            return None
        try:
            # VACIAR ANTES DE PREGUNTAR, Y QUEDARSE CON EL ULTIMO.
            #
            # El nodo manda su estado por su cuenta cada pocos segundos, y
            # ademas contesta a CMD_ESTADO con mas de una linea. Con la conexion
            # mantenida esos sobrantes se apilan, asi que leer "el primer 0x85
            # que llegue" devolvia una foto vieja — la misma foto una y otra
            # vez. Los incrementos salian a cero, y como solo se imprime cuando
            # algo cambia, el monitor no dijo NADA en una hora entera. El
            # silencio parecia normalidad. Esto es el precio de mantener la
            # conexion abierta, y la otra mitad del arreglo.
            for _ in self.c.lee(0.05):
                pass
            self.c.manda(0x05)              # CMD_ESTADO
            ultimo = None
            for t, p in self.c.lee(1.5):
                if t == 0x85:
                    ultimo = p
            if ultimo is not None:
                campos = {}
                for tk in ultimo.decode('utf-8', 'replace').split():
                    if '=' in tk:
                        k, _, v = tk.partition('=')
                        campos[k] = v
                return campos
        except Exception as e:
            self._suelta(type(e).__name__)
            return None
        # Conexion viva pero muda: el tubo puede estar caido sin dar error.
        self._suelta('sin respuesta')
        return None


def main():
    a = sys.argv[1:]

    def opt(n, c, d):
        return c(a[a.index(n) + 1]) if n in a else d

    tcp = opt('--tcp', str, '192.168.1.50')
    puerto = opt('--puerto', str, '')
    celda = opt('--celda', int, 0)          # puerto local del tunel al relevo
    cada = opt('--celda-cada', float, 60.0)

    diario = Diario()
    n = Nodo(puerto) if puerto else Nodo(tcp=tcp)
    time.sleep(1.0)
    diario.apunta('== monitor en marcha sobre %s ==' % (puerto or tcp))
    relevo = Relevo(celda, diario) if celda else None
    if celda:
        diario.apunta('== vigilando ademas la celda por el relevo (127.0.0.1:%d) ==' % celda)

    en_curso = {}
    prev = None
    t_celda = 0.0
    callados = 0                  # sondeos seguidos de la celda sin novedad
    t_latido = time.time()
    t_algo = time.time()          # ultima vez que se oyo CUALQUIER cosa
    while True:
        for t, p in n.lee(1.0):
            t_algo = time.time()
            if t == 0x81 and len(p) >= 8:                   # INICIO
                clave = (p[2:5].hex(), p[5])
                quien = p[7:].decode('ascii', 'replace').strip()
                rssi = int.from_bytes(p[0:1], 'big', signed=True)
                snr = int.from_bytes(p[1:2], 'big', signed=True)
                en_curso[clave] = {'ind': quien, 'modo': p[6], 'lotes': {},
                                   'rssi': rssi, 'snr': snr, 't': time.time()}
                diario.apunta('>> HABLA %s   rssi=%d snr=%d' % (quien, rssi, snr))
            elif t == 0x82 and len(p) > 9:                   # VOZ
                clave = (p[2:5].hex(), p[5])
                d = en_curso.setdefault(clave, {'ind': p[2:5].hex(), 'modo': p[7],
                                                'lotes': {}, 'rssi': 0, 'snr': 0,
                                                't': time.time()})
                d['lotes'][p[6]] = p[9:]
                d['t'] = time.time()
            elif t == 0x83 and len(p) >= 4:                  # FIN
                d = en_curso.pop((p[0:3].hex(), p[3]), None)
                if d and d['lotes']:
                    guarda_audio(d, diario)
            elif t == 0x84 and len(p) >= 9:                  # HOLA
                cola = p[8:].split(b'\0')
                ind = cola[0].decode('ascii', 'replace').strip()
                nom = cola[1].decode('ascii', 'replace').strip() if len(cola) > 1 else ''
                papel = 'CELDA' if p[6] & 0x04 else ('repetidor' if p[6] & 0x01 else 'nodo')
                diario.apunta('   baliza %-24s %-9s bat=%3d%%  rssi=%d'
                              % ('%s (%s)' % (nom, ind) if nom else ind,
                                 papel, p[7], int.from_bytes(p[0:1], 'big', signed=True)))

        # CIERRE POR SILENCIO. El FIN es un paquete mas y se pierde como
        # cualquier otro: si el que habla se mete en una sombra justo al soltar
        # el PTT, el FIN no llega y la transmision se quedaba abierta PARA
        # SIEMPRE — con su audio dentro, sin escribir el WAV. Perdimos asi dos
        # transmisiones el 8-sep-2026, y precisamente las interesantes: las del
        # borde de cobertura. Los lotes van cada 480 ms, asi que 5 s sin uno
        # es que aquello se acabo.
        for clave in [k for k, d in en_curso.items()
                      if time.time() - d.get('t', 0) > CORTE_S]:
            d = en_curso.pop(clave)
            if d['lotes']:
                guarda_audio(d, diario, cortada=True)

        # ⚠️ LATIDO OBLIGATORIO, Y NO ES OPCIONAL.
        #
        # El nodo cierra los clientes WiFi que llevan **3 minutos sin decir
        # nada** (`suelta_tcp_fantasma` en el firmware): sin eso, un movil que
        # se va de cobertura dejaria su ranura ocupada para siempre. Pero este
        # monitor SOLO ESCUCHA — nunca manda nada — asi que el nodo lo tomaba
        # por fantasma y lo echaba a los tres minutos.
        # Y lo peor es como fallaba: el tubo caido **no da error**, `lee()`
        # devuelve vacio, y el monitor se quedaba SORDO EN SILENCIO apuntando
        # solo los contadores de la celda (que van por otra conexion). Paso el
        # 8-sep-2026 y no se noto hasta media hora despues.
        # Un latido cada 30 s, que es lo que hace la app.
        if time.time() - t_latido > 30:
            t_latido = time.time()
            try:
                n.manda(0x05)                  # CMD_ESTADO
            except Exception:
                pass

        # Y si aun asi no llega NADA en cinco minutos, el enlace esta muerto:
        # se rehace. Con balizas cada minuto de tres nodos, cinco minutos de
        # silencio absoluto no son un canal tranquilo, son un tubo roto.
        if time.time() - t_algo > 300:
            diario.apunta('!! cinco minutos sin oir NADA: rehaciendo el enlace')
            try:
                n.s.close()
            except Exception:
                pass
            try:
                n = Nodo(puerto) if puerto else Nodo(tcp=tcp)
                time.sleep(1.0)
                diario.apunta('== enlace rehecho ==')
            except Exception as e:
                diario.apunta('!! no se pudo rehacer: %s' % e)
                time.sleep(5)
            t_algo = time.time()
            t_latido = time.time()

        if celda and time.time() - t_celda > cada:
            t_celda = time.time()
            c = relevo.contadores()
            if c:
                if not prev:
                    # La PRIMERA lectura no tiene con que compararse, pero es
                    # justo la que dice si la celda esta viva. Antes no se
                    # imprimia y el arranque parecia mudo.
                    diario.apunta('   [celda] a la vista: rx=%s rep=%s mal=%s'
                                  % (c.get('rx', '?'), c.get('rep', '?'),
                                     c.get('mal', '?')))
                else:
                    d_rx = int(c.get('rx', 0)) - int(prev.get('rx', 0))
                    d_rep = int(c.get('rep', 0)) - int(prev.get('rep', 0))
                    d_mal = int(c.get('mal', 0)) - int(prev.get('mal', 0))
                    if d_rx or d_rep or d_mal:
                        diario.apunta('   [celda] +%d recibidas, +%d repetidas, +%d corruptas'
                                      % (d_rx, d_rep, d_mal))
                        callados = 0
                    else:
                        # LATIDO DE LOS SONDEOS CALLADOS. Con la celda ociosa
                        # (una baliza por minuto y poco mas) los incrementos son
                        # cero casi siempre, y "no imprimir nada" era
                        # indistinguible de "he perdido el relevo". Cada diez
                        # sondeos callados se dice que sigue ahi y con que
                        # cuentas, que es una linea cada diez minutos.
                        callados += 1
                        if callados >= 10:
                            callados = 0
                            diario.apunta('   [celda] sigue a la vista, sin trafico: '
                                          'rx=%s rep=%s mal=%s'
                                          % (c.get('rx', '?'), c.get('rep', '?'),
                                             c.get('mal', '?')))
                prev = c


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print('\n-- monitor parado')
