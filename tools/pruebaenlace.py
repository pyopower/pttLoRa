#!/usr/bin/env python3
# pruebaenlace.py — prueba del ENLACE POR INTERNET entre dos nodos.
#
#   ./pruebaenlace.py --a 192.168.4.1 --b /dev/ttyACM1
#
# Une por red dos zonas de cobertura LoRa que no se oyen entre si. Por el
# enlace no viajan ordenes, viajan TRAMAS DEL AIRE tal cual: el nodo del otro
# lado las mete por su `procesar()` como si las hubiera oido por la antena, y
# hereda gratis el dedupe, la entrega a sus clientes y la repeticion por radio.
#
# LA PRUEBA SOLO VALE SI LAS DOS PLACAS NO PUEDEN OIRSE POR RADIO. Por eso se
# comprueba antes que estan en frecuencias distintas: si estuvieran en la misma,
# la voz podria haber llegado por el aire y no se demostraria nada.

import math
import os
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nodo import (Nodo, CMD_INICIO, CMD_VOZ, CMD_FIN, CMD_IDENT, CMD_ESTADO,
                  LOTE_MS, codifica)

EV_INICIO, EV_VOZ, EV_FIN, EV_ESTADO, EV_LOG = 0x81, 0x82, 0x83, 0x85, 0x8F


def tono(segundos, hz=700):
    return b''.join(struct.pack('<h', int(9000 * math.sin(
        2 * math.pi * hz * i / 8000.0))) for i in range(int(8000 * segundos)))


class Oyente(threading.Thread):
    def __init__(self, nodo, segundos):
        super().__init__(daemon=True)
        self.nodo, self.segundos, self.eventos = nodo, segundos, []

    def run(self):
        for t, p in self.nodo.lee(self.segundos):
            self.eventos.append((t, p))

    def de_tipo(self, tipo):
        return [p for t, p in self.eventos if t == tipo]


def rssi_de(p):
    return p[0] - 256 if p[0] > 127 else p[0]


def estado(n):
    n.manda(CMD_ESTADO)
    for t, p in n.lee(2.0):
        if t == EV_ESTADO:
            return p.decode('utf8', 'replace')
    return ''


def frecuencia_de(txt):
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

    a_host = opt('--a', '192.168.4.1')
    b_puerto = opt('--b', '/dev/ttyACM1')

    a = Nodo(tcp=a_host)
    b = Nodo(b_puerto)
    time.sleep(0.5)

    ea, eb = estado(a), estado(b)
    fa, fb = frecuencia_de(ea), frecuencia_de(eb)
    print('nodo A: %s' % ea[:60])
    print('        %s' % ea[ea.index('wifi='):] if 'wifi=' in ea else '')
    print('nodo B: %s' % eb[:60])
    print('        %s' % eb[eb.index('wifi='):] if 'wifi=' in eb else '')
    print()

    if fa is None or fb is None or abs(fa - fb) < 0.5:
        print('LAS DOS PLACAS ESTAN EN LA MISMA FRECUENCIA (%s / %s): la prueba'
              ' no demostraria nada, porque la voz podria llegar por el aire.'
              % (fa, fb))
        return 1
    print('frecuencias distintas (%.3f vs %.3f): por radio NO pueden oirse.'
          % (fa, fb))
    if 'enlace=' not in eb and 'enlace=' not in ea:
        print('AVISO: ningun nodo declara enlace.')

    fallos = []
    cod, n_tramas, lotes = codifica(tono(2.5), '1200')

    # ---------------------------------------------------------- A hacia B --
    print()
    print('== un cliente de A habla; B tiene que oirlo POR EL ENLACE ==')
    a.manda(CMD_IDENT, b'EA3AAA')
    time.sleep(0.3)
    oye_b = Oyente(b, 8); oye_b.start()
    time.sleep(0.5)
    a.manda(CMD_INICIO, bytes([cod]))
    for lote in lotes:
        a.manda(CMD_VOZ, bytes([cod, n_tramas]) + lote)
        time.sleep(LOTE_MS / 1000.0)
    a.manda(CMD_FIN)
    oye_b.join()

    voz = oye_b.de_tipo(EV_VOZ)
    ini = oye_b.de_tipo(EV_INICIO)
    inds = set(p[7:].decode('ascii', 'replace') for p in ini)
    print('B recibio %d lotes de %s' % (len(voz), ','.join(sorted(inds)) or '-'))
    if len(voz) < len(lotes) - 1:
        fallos.append('B recibio %d lotes de %d' % (len(voz), len(lotes)))
    if 'EA3AAA' not in inds:
        fallos.append('B no vio el indicativo del cliente de A (vio %s)' % inds)

    # ---------------------------------------------------------- B hacia A --
    print()
    print('== ahora habla B; el cliente de A tiene que oirlo POR EL ENLACE ==')
    b.manda(CMD_IDENT, b'EA3BBB')
    time.sleep(0.3)
    oye_a = Oyente(a, 8); oye_a.start()
    time.sleep(0.5)
    b.manda(CMD_INICIO, bytes([cod]))
    for lote in lotes:
        b.manda(CMD_VOZ, bytes([cod, n_tramas]) + lote)
        time.sleep(LOTE_MS / 1000.0)
    b.manda(CMD_FIN)
    oye_a.join()

    voz = oye_a.de_tipo(EV_VOZ)
    ini = oye_a.de_tipo(EV_INICIO)
    inds = set(p[7:].decode('ascii', 'replace') for p in ini)
    print('el cliente de A recibio %d lotes de %s'
          % (len(voz), ','.join(sorted(inds)) or '-'))
    if len(voz) < len(lotes) - 1:
        fallos.append('el cliente de A recibio %d lotes de %d'
                      % (len(voz), len(lotes)))
    if 'EA3BBB' not in inds:
        fallos.append('A no vio el indicativo de B (vio %s)' % inds)

    print()
    if fallos:
        print('FALLOS:')
        for f in fallos:
            print(' -', f)
        return 1
    print('TODO BIEN: la voz cruzo por Internet entre dos nodos que no se oyen')
    return 0


if __name__ == '__main__':
    sys.exit(main())
