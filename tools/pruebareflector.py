#!/usr/bin/env python3
# pruebareflector.py — el arbitraje global del reflector, con dos que pisan.
#
#   ./pruebareflector.py
#
# Cada nodo bloquea el PTT cuando oye el canal ocupado, pero eso solo tapa las
# colisiones que le da tiempo a ver: dos personas en ubicaciones distintas que
# pulsan a la vez no se enteran la una de la otra hasta que la voz ha cruzado.
# El reflector si las ve a las dos, y se queda con la primera.
#
# Aqui se montan tres miembros de mentira (dos que hablan y uno que escucha) y
# se comprueba que el que escucha recibe UNA transmision entera y limpia, no las
# dos entrelazadas.

import os
import socket
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nodovirtual import CMD_AIRE, CAB_LEN, Desentrama, enmarcar

PUERTO = 44610          # uno cualquiera, para no chocar con nodos de verdad
MAGIC = 0xA1
T_VOZ, T_INICIO, T_FIN = 1, 2, 3


def trama(tipo, src, stream, seq, cuerpo=b''):
    return bytes([MAGIC, 1, (tipo << 4) | 3,
                  (src >> 16) & 255, (src >> 8) & 255, src & 255,
                  stream, seq]) + cuerpo


def habla(s, src, stream, indicativo, lotes, retardo=0.0):
    """Una transmisión completa, al ritmo real de un lote cada 480 ms."""
    time.sleep(retardo)
    s.sendall(enmarcar(CMD_AIRE, trama(T_INICIO, src, stream, 0,
                                       bytes([4]) + indicativo)))
    for i in range(lotes):
        s.sendall(enmarcar(CMD_AIRE, trama(T_VOZ, src, stream, i + 1,
                                           bytes([4, 12]) + b'\x55' * 72)))
        time.sleep(0.48)
    s.sendall(enmarcar(CMD_AIRE, trama(T_FIN, src, stream, lotes + 1)))


def main():
    aqui = os.path.dirname(os.path.abspath(__file__))
    # A fichero y no a un pipe: nadie lo lee mientras corre la prueba, y un
    # pipe lleno dejaria al reflector bloqueado escribiendo.
    reg = os.path.join(os.environ.get('TMPDIR', '/tmp'), 'pttlora-reflector.log')
    salida_f = open(reg, 'w')
    refl = subprocess.Popen([sys.executable, '-u',
                             os.path.join(aqui, 'nodovirtual.py'),
                             '--escucha', str(PUERTO)],
                            stdout=salida_f, stderr=subprocess.STDOUT)
    time.sleep(1.0)
    try:
        a = socket.create_connection(('127.0.0.1', PUERTO), 3)
        b = socket.create_connection(('127.0.0.1', PUERTO), 3)
        oyente = socket.create_connection(('127.0.0.1', PUERTO), 3)
        oyente.settimeout(None)
        time.sleep(0.3)

        recibidas = []

        def escucha():
            d = Desentrama()
            while True:
                try:
                    dat = oyente.recv(4096)
                except OSError:
                    return
                if not dat:
                    return
                for tipo, p in d.mete(dat):
                    if tipo == CMD_AIRE and len(p) >= CAB_LEN:
                        recibidas.append(p)

        threading.Thread(target=escucha, daemon=True).start()

        print('== EA3AAA y EA3BBB pulsan casi a la vez, en sitios distintos ==')
        h1 = threading.Thread(target=habla,
                              args=(a, 0xAAAAAA, 11, b'EA3AAA', 5, 0.0))
        h2 = threading.Thread(target=habla,
                              args=(b, 0xBBBBBB, 22, b'EA3BBB', 5, 0.15))
        h1.start(); h2.start(); h1.join(); h2.join()
        time.sleep(1.0)

        # Solo la VOZ: el FIN del segundo llega cuando el primero ya ha
        # terminado, y entonces pasar es lo correcto, no una colision.
        fuentes = {}
        for p in recibidas:
            if (p[2] >> 4) != T_VOZ:
                continue
            src = (p[3] << 16) | (p[4] << 8) | p[5]
            fuentes[src] = fuentes.get(src, 0) + 1
        print('el que escucha recibio: %s'
              % ', '.join('%06x=%d tramas' % (k, v) for k, v in fuentes.items()))

        fallos = []
        if len(fuentes) != 1:
            fallos.append('llegaron %d transmisiones entrelazadas, deberia ser 1'
                          % len(fuentes))
        elif 0xAAAAAA not in fuentes:
            fallos.append('paso la segunda en vez de la primera')
        elif fuentes[0xAAAAAA] < 5:
            fallos.append('la que paso llego incompleta (%d de 5 lotes)'
                          % fuentes[0xAAAAAA])

        # Y ahora, con el canal libre, el segundo tiene que poder hablar.
        print('== y cuando el primero termina, el segundo si pasa ==')
        recibidas.clear()
        habla(b, 0xBBBBBB, 23, b'EA3BBB', 2)
        time.sleep(1.0)
        srcs = set((p[3] << 16) | (p[4] << 8) | p[5] for p in recibidas)
        print('el que escucha recibio ahora: %s'
              % ', '.join('%06x' % s for s in srcs))
        if 0xBBBBBB not in srcs:
            fallos.append('el segundo no pudo hablar con el canal ya libre')

        print()
        if fallos:
            print('FALLOS:')
            for f in fallos:
                print(' -', f)
            return 1
        print('TODO BIEN: el reflector se queda con el primero y no deja pasar '
              'lo que le pisa')
        return 0
    finally:
        refl.terminate()
        try:
            refl.wait(timeout=3)
        except subprocess.TimeoutExpired:
            refl.kill()
        salida_f.close()
        texto = open(reg).read()
        print('(el reflector descarto %d tramas del que llego tarde; %s)'
              % (texto.count(' XX '), reg))


if __name__ == '__main__':
    sys.exit(main())
