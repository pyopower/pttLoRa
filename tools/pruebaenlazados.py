#!/usr/bin/env python3
"""Banco del enlace entre reflectores (`nodovirtual.py --enlace`).

    ./pruebaenlazados.py

Levanta reflectores de verdad en puertos altos de 127.0.0.1 y comprueba:

  1. AMBOS: lo que habla una red propia llega al principal, y al reves.
  2. RECIBIR: la red propia oye al principal, pero lo suyo no sale.
  3. CIRCULO: A enlazado a B y B enlazado a A (mal configurado a proposito).
     Una trama llega, como mucho duplicada, y NO se queda dando vueltas.
  4. CAIDA: se cae el principal, vuelve, y el enlace se rehace solo.
"""
import os
import socket
import subprocess
import sys
import threading
import time

AQUI = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, AQUI)
from nodovirtual import enmarcar, Desentrama, CMD_AIRE  # noqa: E402

T_VOZ = 1
fallos = []


def trama(src, stream, seq):
    return bytes([0xA1, 1, (T_VOZ << 4) | 3, (src >> 16) & 255, (src >> 8) & 255,
                  src & 255, stream, seq]) + b'voz-de-prueba'


def arranca(puerto, *extra):
    return subprocess.Popen([sys.executable, os.path.join(AQUI, 'nodovirtual.py'),
                             '--escucha', str(puerto)] + list(extra),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class Oyente:
    """Un cliente que cuenta lo que le llega."""
    def __init__(self, puerto):
        for _ in range(50):
            try:
                self.s = socket.create_connection(('127.0.0.1', puerto), 1)
                break
            except OSError:
                time.sleep(0.1)
        self.s.settimeout(None)
        self.recibidas = []
        threading.Thread(target=self._lee, daemon=True).start()

    def _lee(self):
        d = Desentrama()
        try:
            while True:
                b = self.s.recv(4096)
                if not b:
                    return
                for tipo, p in d.mete(b):
                    if tipo == CMD_AIRE:
                        self.recibidas.append(p)
        except OSError:
            pass

    def manda(self, t):
        self.s.sendall(enmarcar(CMD_AIRE, t))

    def cuenta(self, t):
        return sum(1 for x in self.recibidas if x == t)

    def cierra(self):
        self.s.close()


def comprueba(nombre, ok, detalle=''):
    print('  %s %s %s' % ('OK  ' if ok else 'FALLO', nombre, detalle))
    if not ok:
        fallos.append(nombre)


def espera_enlace(s=1.5):
    time.sleep(s)


def prueba_ambos():
    print('1. enlace en los dos sentidos')
    p = arranca(24481)
    time.sleep(0.5)
    l = arranca(24482, '--enlace', '127.0.0.1:24481')
    try:
        lejos, cerca = Oyente(24481), Oyente(24482)
        espera_enlace()
        t1 = trama(0x111111, 1, 0)
        cerca.manda(t1)
        time.sleep(0.5)
        comprueba('lo de la red propia llega al principal', lejos.cuenta(t1) == 1,
                  '(%d)' % lejos.cuenta(t1))
        time.sleep(2.2)                       # que caduque el turno
        t2 = trama(0x222222, 2, 0)
        lejos.manda(t2)
        time.sleep(0.5)
        comprueba('lo del principal llega a la red propia', cerca.cuenta(t2) == 1,
                  '(%d)' % cerca.cuenta(t2))
        comprueba('a nadie le vuelve lo suyo',
                  cerca.cuenta(t1) == 0 and lejos.cuenta(t2) == 0)
    finally:
        l.kill(); p.kill()


def prueba_recibir():
    print('2. solo recibir')
    p = arranca(24483)
    time.sleep(0.5)
    l = arranca(24484, '--enlace', '127.0.0.1:24483', '--enlace-modo', 'recibir')
    try:
        lejos, cerca, vecino = Oyente(24483), Oyente(24484), Oyente(24484)
        espera_enlace()
        t1 = trama(0x333333, 3, 0)
        cerca.manda(t1)
        time.sleep(0.5)
        comprueba('lo de aqui NO sale', lejos.cuenta(t1) == 0)
        comprueba('...pero en casa se oye', vecino.cuenta(t1) == 1)
        time.sleep(2.2)
        t2 = trama(0x444444, 4, 0)
        lejos.manda(t2)
        time.sleep(0.5)
        comprueba('lo de fuera SI entra', cerca.cuenta(t2) == 1)
    finally:
        l.kill(); p.kill()


def prueba_circulo():
    print('3. circulo A<->B (mal configurado)')
    a = arranca(24485, '--enlace', '127.0.0.1:24486')
    b = arranca(24486, '--enlace', '127.0.0.1:24485')
    try:
        en_a, en_b = Oyente(24485), Oyente(24486)
        espera_enlace(2.5)
        t = trama(0x555555, 5, 0)
        en_a.manda(t)
        time.sleep(1.0)
        n1 = en_b.cuenta(t)
        time.sleep(2.0)
        n2 = en_b.cuenta(t)
        comprueba('llega al otro lado', n1 >= 1, '(%d)' % n1)
        comprueba('como mucho duplicada', n2 <= 2, '(%d)' % n2)
        comprueba('y no sigue dando vueltas', n2 == n1, '(%d -> %d)' % (n1, n2))
        comprueba('al que hablo no le vuelve en bucle', en_a.cuenta(t) <= 1,
                  '(%d)' % en_a.cuenta(t))
    finally:
        a.kill(); b.kill()


def prueba_caida():
    print('4. se cae el principal y vuelve')
    p = arranca(24487)
    time.sleep(0.5)
    l = arranca(24488, '--enlace', '127.0.0.1:24487')
    try:
        cerca = Oyente(24488)
        espera_enlace()
        p.kill(); p.wait()
        time.sleep(1.0)
        p = arranca(24487)
        lejos = Oyente(24487)
        # primer reintento a los 5 s tras la caida
        t = trama(0x666666, 6, 0)
        llego = False
        for i in range(20):
            time.sleep(1.0)
            cerca.manda(trama(0x666666, 6, i))
            time.sleep(0.2)
            if any(x[3:6] == t[3:6] for x in lejos.recibidas):
                llego = True
                break
        comprueba('el enlace se rehace solo', llego, '(%d s)' % (i + 1))
    finally:
        l.kill(); p.kill()


if __name__ == '__main__':
    prueba_ambos()
    prueba_recibir()
    prueba_circulo()
    prueba_caida()
    print()
    print('RESULTADO: %s' % ('TODO OK' if not fallos else
                             'FALLAN %d: %s' % (len(fallos), ', '.join(fallos))))
    sys.exit(1 if fallos else 0)
