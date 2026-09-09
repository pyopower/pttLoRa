#!/usr/bin/env python3
# pruebawifi.py — prueba de VARIOS USUARIOS colgados de un mismo nodo por WiFi.
#
#   ./pruebawifi.py 192.168.4.1 [--radio /dev/ttyACM1]
#
# Comprueba las tres cosas que hacen falta para que un nodo de servicio a un
# grupo (el caso de la excursion: el nodo va en la mochila y levanta su propio
# punto de acceso):
#
#   1. Cada cliente emite con SU indicativo, no con el del nodo. En banda de
#      aficionado no se emite bajo indicativo ajeno.
#   2. Los clientes se oyen ENTRE ELLOS. Por radio no podrian: el nodo no
#      escucha sus propias emisiones, asi que la voz del que habla se les
#      devuelve por dentro (eco local, marcado con rssi=127, imposible por LoRa).
#   3. Solo habla uno: al segundo que pulse se le contesta quien tiene el
#      microfono, en vez de dejarle un PTT que no hace nada.
#
# Con --radio se comprueba ademas que ESO MISMO sale por la antena y lo recibe
# otro nodo de verdad, que es lo que separa esto de un bucle local.

import math
import os
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nodo import (Nodo, CMD_INICIO, CMD_VOZ, CMD_FIN, CMD_IDENT, CMD_ESTADO,
                  MODOS, LOTE_MS, codifica)

EV_INICIO, EV_VOZ, EV_FIN, EV_ESTADO, EV_PTT, EV_LOG = (
    0x81, 0x82, 0x83, 0x85, 0x89, 0x8F)
PTT_LIBRE, PTT_TUYO, PTT_DE_OTRO = 0, 1, 2


def tono(segundos, hz=700):
    return b''.join(struct.pack('<h', int(9000 * math.sin(
        2 * math.pi * hz * i / 8000.0))) for i in range(int(8000 * segundos)))


class Oyente(threading.Thread):
    """Recoge en segundo plano todo lo que el nodo le manda a un cliente."""

    def __init__(self, nodo, nombre, segundos):
        super().__init__(daemon=True)
        self.nodo, self.nombre, self.segundos = nodo, nombre, segundos
        self.eventos = []

    def run(self):
        for t, p in self.nodo.lee(self.segundos):
            self.eventos.append((t, p))

    def de_tipo(self, tipo):
        return [p for t, p in self.eventos if t == tipo]


def rssi_de(p):
    """Primer byte de EV_INICIO/EV_VOZ, con signo. 127 = no vino del aire."""
    return p[0] - 256 if p[0] > 127 else p[0]


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip())
        return 1
    host = sys.argv[1]
    radio = None
    if '--radio' in sys.argv:
        radio = sys.argv[sys.argv.index('--radio') + 1]

    print('== conectando tres clientes al mismo nodo por WiFi ==')
    a = Nodo(tcp=host); a.manda(CMD_IDENT, b'EA3AAA')
    b = Nodo(tcp=host); b.manda(CMD_IDENT, b'EA3BBB')
    c = Nodo(tcp=host); c.manda(CMD_IDENT, b'EA3CCC')
    time.sleep(0.5)

    a.manda(CMD_ESTADO)
    for t, p in a.lee(1.5):
        if t == EV_ESTADO:
            print('   nodo:', p.decode('utf8', 'replace')[-90:])

    # El nodo de al lado, escuchando POR RADIO. Sin esto la prueba solo
    # demostraria que el nodo se habla a si mismo.
    vecino = oye_vecino = None
    if radio:
        vecino = Nodo(radio)
        oye_vecino = Oyente(vecino, 'radio', 14)
        oye_vecino.start()

    secs = 4.0
    cod, n_tramas, lotes = codifica(tono(secs), '1200')
    print('== EA3AAA habla %.0f s (%d lotes); EA3BBB y EA3CCC escuchan ==' %
          (secs, len(lotes)))

    oye_b = Oyente(b, 'B', 12); oye_b.start()
    oye_c = Oyente(c, 'C', 12); oye_c.start()
    oye_a = Oyente(a, 'A', 12); oye_a.start()

    a.manda(CMD_INICIO, bytes([cod]))
    t0 = time.time()

    # A los 2 lotes, B intenta hablar encima: tiene que rebotar.
    for i, lote in enumerate(lotes):
        if i == 2:
            print('   ...EA3BBB intenta pulsar el PTT mientras EA3AAA habla')
            b.manda(CMD_INICIO, bytes([cod]))
        a.manda(CMD_VOZ, bytes([cod, n_tramas]) + lote)
        espera = t0 + (i + 1) * LOTE_MS / 1000.0 - time.time()
        if espera > 0:
            time.sleep(espera)
    a.manda(CMD_FIN)
    print('   ...EA3AAA suelta')
    time.sleep(2.0)

    # Y ahora que esta libre, que hable B: el PTT tiene que soltarse solo.
    print('== ahora habla EA3BBB (el PTT deberia estar libre) ==')
    b.manda(CMD_INICIO, bytes([cod]))
    for lote in lotes[:3]:
        b.manda(CMD_VOZ, bytes([cod, n_tramas]) + lote)
        time.sleep(LOTE_MS / 1000.0)
    b.manda(CMD_FIN)

    for h in (oye_a, oye_b, oye_c):
        h.join()
    if oye_vecino:
        oye_vecino.join()

    # ---- y al reves: el nodo VECINO habla por radio y los tres clientes del
    # nodo tienen que oirle. Es la otra mitad del asunto: no basta con que se
    # oigan entre ellos, tienen que oir la malla.
    remoto = {}
    if vecino:
        print()
        print('== ahora habla el nodo vecino POR RADIO; los 3 clientes escuchan ==')
        vecino.manda(CMD_IDENT, b'EA3REM')
        time.sleep(0.3)
        oyes = {'EA3AAA': Oyente(a, 'A', 9), 'EA3BBB': Oyente(b, 'B', 9),
                'EA3CCC': Oyente(c, 'C', 9)}
        for o in oyes.values():
            o.start()
        time.sleep(0.5)
        vecino.manda(CMD_INICIO, bytes([cod]))
        for lote in lotes[:5]:
            vecino.manda(CMD_VOZ, bytes([cod, n_tramas]) + lote)
            time.sleep(LOTE_MS / 1000.0)
        vecino.manda(CMD_FIN)
        for o in oyes.values():
            o.join()
        remoto = oyes

    # ------------------------------------------------------------ veredicto --
    print()
    fallos = []

    def revisa(cual, oye, espera_voz, quien):
        voz = oye.de_tipo(EV_VOZ)
        ini = oye.de_tipo(EV_INICIO)
        indicativos = set(p[7:].decode('ascii', 'replace') for p in ini)
        locales = sum(1 for p in voz if rssi_de(p) == 127)
        print('%s: %d INICIO de %s, %d lotes de voz (%d por dentro del nodo)'
              % (cual, len(ini), ','.join(sorted(indicativos)) or '-',
                 len(voz), locales))
        if espera_voz and len(voz) < espera_voz:
            fallos.append('%s recibio %d lotes, esperaba >=%d'
                          % (cual, len(voz), espera_voz))
        if quien and quien not in indicativos:
            fallos.append('%s no vio el indicativo %s (vio %s)'
                          % (cual, quien, indicativos))

    # B y C tienen que haber oido a A por dentro, aunque por radio no podrian.
    revisa('EA3BBB', oye_b, len(lotes), 'EA3AAA')
    revisa('EA3CCC', oye_c, len(lotes) + 3, 'EA3AAA')
    # A no debe oirse a si mismo.
    voz_a = [p for p in oye_a.de_tipo(EV_VOZ)]
    print('EA3AAA: %d lotes recibidos (deberian ser los 3 de EA3BBB, no los '
          'suyos)' % len(voz_a))
    if len(voz_a) > 3:
        fallos.append('EA3AAA se oye a si mismo (%d lotes)' % len(voz_a))

    # El arbitraje.
    rebotes = [p for p in oye_b.de_tipo(EV_PTT) if p and p[0] == PTT_DE_OTRO]
    if rebotes:
        print('arbitraje: a EA3BBB se le dijo "habla %s"'
              % rebotes[0][1:].decode('ascii', 'replace'))
    else:
        fallos.append('a EA3BBB no se le aviso de que el PTT estaba cogido')
    if not any(p and p[0] == PTT_TUYO for p in oye_b.de_tipo(EV_PTT)):
        fallos.append('EA3BBB no llego a coger el PTT en su turno')

    # Y por radio, de verdad.
    if oye_vecino:
        voz_r = oye_vecino.de_tipo(EV_VOZ)
        ini_r = oye_vecino.de_tipo(EV_INICIO)
        inds = set(p[7:].decode('ascii', 'replace') for p in ini_r)
        aire = [p for p in voz_r if rssi_de(p) != 127]
        print('por RADIO el nodo vecino oyo: %d lotes de %s, rssi %s dBm'
              % (len(aire), ','.join(sorted(inds)) or '-',
                 rssi_de(aire[0]) if aire else '?'))
        if len(aire) < len(lotes) // 2:
            fallos.append('el vecino apenas oyo nada por radio (%d lotes)'
                          % len(aire))
        if 'EA3AAA' not in inds:
            fallos.append('por radio no salio el indicativo del CLIENTE '
                          '(vio %s)' % inds)

    for cual, o in remoto.items():
        voz = o.de_tipo(EV_VOZ)
        ini = o.de_tipo(EV_INICIO)
        inds = set(p[7:].decode('ascii', 'replace') for p in ini)
        aire = [p for p in voz if rssi_de(p) != 127]
        print('%s oyo del nodo remoto: %d lotes de %s, rssi %s dBm'
              % (cual, len(aire), ','.join(sorted(inds)) or '-',
                 rssi_de(aire[0]) if aire else '?'))
        if len(aire) < 4:
            fallos.append('%s apenas oyo al nodo remoto (%d lotes)'
                          % (cual, len(aire)))
        if 'EA3REM' not in inds:
            fallos.append('%s no vio el indicativo del nodo remoto (vio %s)'
                          % (cual, inds))

    print()
    if fallos:
        print('FALLOS:')
        for f in fallos:
            print(' -', f)
        return 1
    print('TODO BIEN')
    return 0


if __name__ == '__main__':
    sys.exit(main())
