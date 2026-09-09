#!/usr/bin/env python3
# pruebareflectorreal.py — dos nodos DE VERDAD unidos por el reflector.
#
#   ./pruebareflectorreal.py --a /dev/ttyACM0 --b /dev/ttyACM1 \
#       --reflector or.adan.ovh
#
# Es la prueba que de verdad cierra el asunto: dos placas que NO se oyen por
# radio (se ponen en frecuencias distintas a proposito) hablando entre ellas a
# traves de un servidor en Internet. Si la voz llega, solo puede haber ido por
# ahi.
#
# Requisitos: las dos placas conectadas a una red WiFi con Internet y con el
# reflector configurado. El script comprueba las dos cosas antes de empezar y se
# niega a seguir si no se cumplen, para no dar por bueno un resultado que en
# realidad venia por el aire.

import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nodo import Nodo, CMD_INICIO, CMD_VOZ, CMD_FIN, CMD_IDENT, CMD_ESTADO, \
                 LOTE_MS, codifica
from pruebawifi import Oyente, tono, rssi_de, EV_INICIO, EV_VOZ, EV_ESTADO


def estado(n):
    n.manda(CMD_ESTADO)
    for t, p in n.lee(3.0):
        if t == EV_ESTADO:
            return p.decode('utf8', 'replace')
    return ''


def frec(txt):
    for pal in txt.split():
        if pal.endswith('MHz') and not pal.startswith('hw='):
            try:
                return float(pal[:-3])
            except ValueError:
                pass
    return None


def main():
    def opt(nombre, defecto=None):
        return (sys.argv[sys.argv.index(nombre) + 1]
                if nombre in sys.argv else defecto)

    pa = opt('--a', '/dev/ttyACM0')
    pb = opt('--b', '/dev/ttyACM1')
    refl = opt('--reflector', 'or.adan.ovh')

    a, b = Nodo(pa), Nodo(pb)
    time.sleep(0.5)
    ea, eb = estado(a), estado(b)
    for nombre, e in (('A', ea), ('B', eb)):
        corte = e.index('wifi=') if 'wifi=' in e else 0
        print('nodo %s: %s' % (nombre, e[:22]))
        print('         %s' % e[corte:])
    print()

    fallos = []
    fa, fb = frec(ea), frec(eb)
    if fa is None or fb is None or abs(fa - fb) < 0.5:
        print('LAS DOS PLACAS ESTAN EN LA MISMA FRECUENCIA (%s / %s).' % (fa, fb))
        print('La prueba no demostraria nada: la voz podria ir por el aire.')
        return 1
    print('frecuencias distintas (%.3f / %.3f): por radio no se oyen.' % (fa, fb))
    for nombre, e in (('A', ea), ('B', eb)):
        if '(en pie)' not in e:
            fallos.append('el nodo %s no tiene el enlace en pie' % nombre)
    if fallos:
        print()
        for f in fallos:
            print(' -', f)
        print('Configura: tools/nodo.py --puerto <x> enlace %s 4461' % refl)
        return 1
    print('los dos nodos enlazados con %s' % refl)

    cod, n_tramas, lotes = codifica(tono(2.5), '1200')
    for emisor, receptor, quien, otro in ((a, b, 'EA3AAA', 'B'), (b, a, 'EA3BBB', 'A')):
        print()
        print('== habla %s; el nodo %s tiene que oirlo POR EL REFLECTOR ==' % (quien, otro))
        emisor.manda(CMD_IDENT, quien.encode())
        time.sleep(0.3)
        oye = Oyente(receptor, otro, 8)
        oye.start()
        time.sleep(0.5)
        emisor.manda(CMD_INICIO, bytes([cod]))
        for lote in lotes:
            emisor.manda(CMD_VOZ, bytes([cod, n_tramas]) + lote)
            time.sleep(LOTE_MS / 1000.0)
        emisor.manda(CMD_FIN)
        oye.join()
        voz = oye.de_tipo(EV_VOZ)
        ini = oye.de_tipo(EV_INICIO)
        inds = set(p[7:].decode('ascii', 'replace') for p in ini)
        print('   %s recibio %d de %d lotes de %s'
              % (otro, len(voz), len(lotes), ','.join(sorted(inds)) or '-'))
        if len(voz) < len(lotes) - 1:
            fallos.append('%s recibio %d de %d lotes' % (otro, len(voz), len(lotes)))
        if quien not in inds:
            fallos.append('%s no vio el indicativo %s' % (otro, quien))

    print()
    if fallos:
        print('FALLOS:')
        for f in fallos:
            print(' -', f)
        return 1
    print('TODO BIEN: dos nodos que no se oyen por radio, hablando por Internet '
          'a traves de %s' % refl)
    return 0


if __name__ == '__main__':
    sys.exit(main())
