#!/usr/bin/env python3
# prueba2nodos.py — prueba de dos nodos en la misma mesa.
#
#   ./prueba2nodos.py [--tx /dev/ttyACM0] [--rx /dev/ttyACM1] [--segundos 4]
#                     [--modo 1200] [--potencia 5]
#
# Mantiene los DOS puertos abiertos todo el rato. Es importante: abrir el
# puerto serie reinicia el ESP32 (DTR/RTS), asi que una prueba hecha con varias
# invocaciones de nodo.py reinicia las placas por el camino y los contadores no
# valen nada.
#
# Al final pregunta el estado por la MISMA conexion, para poder distinguir
# "la radio no recibio nada" de "recibio pero no llego al anfitrion".

import os, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nodo import Nodo, CMD_CONFIG, CMD_ESTADO, CMD_INICIO, CMD_VOZ, CMD_FIN, \
                 codifica, pinta


def main():
    a = sys.argv[1:]

    def opt(n, c, d):
        return c(a[a.index(n) + 1]) if n in a else d

    p_tx = opt('--tx', str, '/dev/ttyACM0')
    p_rx = opt('--rx', str, '/dev/ttyACM1')
    secs = opt('--segundos', float, 4.0)
    modo = opt('--modo', str, '1200')
    pot = opt('--potencia', int, 2)

    print('-- abriendo los dos puertos (esto reinicia las dos placas)')
    tx = Nodo(p_tx)
    rx = Nodo(p_rx)
    time.sleep(2.5)                      # que arranquen del reset

    tx.manda(CMD_CONFIG, bytes([1, 3, pot]) + b'NOCALL')
    rx.manda(CMD_CONFIG, bytes([1, 3, pot]) + b'NOCALL-7')
    time.sleep(0.5)

    recibido = []
    parar = threading.Event()

    def escucha():
        while not parar.is_set():
            for t, p in rx.lee(0.5):
                recibido.append((t, p))
                pinta(t, p)

    hilo = threading.Thread(target=escucha, daemon=True)
    hilo.start()
    time.sleep(1.0)

    import math, struct
    raw = b''.join(struct.pack('<h', int(9000 * math.sin(
        2 * math.pi * 700 * i / 8000.0))) for i in range(int(8000 * secs)))
    cod, n_tramas, lotes = codifica(raw, modo)
    print('-- TX: %s, %d lotes de %d B' % (modo, len(lotes), len(lotes[0])))
    tx.manda(CMD_INICIO, bytes([cod]))
    t0 = time.time()
    for i, lote in enumerate(lotes):
        tx.manda(CMD_VOZ, bytes([cod, n_tramas]) + lote)
        espera = t0 + (i + 1) * 0.480 - time.time()
        if espera > 0:
            time.sleep(espera)
    tx.manda(CMD_FIN)
    print('-- TX terminada (%d lotes)' % len(lotes))

    time.sleep(2.5)
    parar.set()
    hilo.join(timeout=2)

    voz = [x for x in recibido if x[0] == 0x82]
    print('\n-- recibidas %d tramas en total, %d de voz' % (len(recibido), len(voz)))

    print('\n-- estado del que TRANSMITE:')
    tx.manda(CMD_ESTADO)
    for t, p in tx.lee(1.5):
        pinta(t, p)
    print('-- estado del que RECIBE:')
    rx.manda(CMD_ESTADO)
    for t, p in rx.lee(1.5):
        pinta(t, p)


if __name__ == '__main__':
    main()
