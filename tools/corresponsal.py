#!/usr/bin/env python3
# corresponsal.py — hace de segunda estación desde la raspi.
#
#   ./corresponsal.py [--puerto /dev/ttyACM0] [--indicativo C31AG] [--minutos 10]
#
# Escucha todo lo que llegue por radio y, cada vez que alguien termina de
# hablar, reconstruye su audio en un .wav. Con `--decir <fichero.wav>` además
# transmite eso al empezar.
#
# Sirve para probar la app con un solo móvil: el móvil habla por su nodo y esto
# recoge por el otro, así se ve si el camino completo funciona sin necesidad de
# dos teléfonos.

import os, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nodo import Nodo, CMD_CONFIG, CMD_INICIO, CMD_VOZ, CMD_FIN, MODOS, codifica

# codigo de modo -> nombre de codec2, para descodificar lo que llegue
POR_CODIGO = {v[0]: k for k, v in MODOS.items()}


def main():
    a = sys.argv[1:]

    def opt(n, c, d):
        return c(a[a.index(n) + 1]) if n in a else d

    puerto = opt('--puerto', str, '/dev/ttyACM0')
    bt = opt('--bt', str, '')
    ind = opt('--indicativo', str, 'C31AG')
    mins = opt('--minutos', float, 10.0)
    decir = opt('--decir', str, '')
    pot = opt('--potencia', int, 17)
    salida = opt('--salida', str, '/home/yo/pttlora/bench/recibido')

    n = Nodo(bt=bt) if bt else Nodo(puerto)
    time.sleep(1.0)
    n.manda(CMD_CONFIG, bytes([1, 3, pot]) + ind.upper().encode())
    time.sleep(0.8)
    print('-- %s en el aire, escuchando %.0f min' % (ind, mins))

    if decir:
        raw_tmp = '/tmp/.corresponsal.raw'
        subprocess.run(['sox', decir, '-r', '8000', '-c', '1', '-b', '16',
                        '-e', 'signed-integer', raw_tmp], check=True)
        cod, nt, lotes = codifica(open(raw_tmp, 'rb').read(), '1200')
        print('-- transmitiendo %d lotes' % len(lotes))
        n.manda(CMD_INICIO, bytes([cod]))
        t0 = time.time()
        for i, l in enumerate(lotes):
            n.manda(CMD_VOZ, bytes([cod, nt]) + l)
            e = t0 + (i + 1) * 0.480 - time.time()
            if e > 0:
                time.sleep(e)
        n.manda(CMD_FIN)
        print('-- soltado')

    # Cada transmision que llega se junta por (src, stream): asi no se mezclan
    # dos estaciones que hablen seguidas.
    en_curso = {}
    fin = time.time() + mins * 60
    while time.time() < fin:
        for t, p in n.lee(1.0):
            if t == 0x81 and len(p) >= 8:               # INICIO
                clave = (p[2:5].hex(), p[5])
                quien = p[7:].decode('ascii', 'replace').strip()
                en_curso[clave] = {'ind': quien, 'modo': p[6], 'lotes': {},
                                   'rssi': int.from_bytes(p[0:1], 'big', signed=True)}
                print('>> habla %s (%d dBm)' % (quien, en_curso[clave]['rssi']))
            elif t == 0x82 and len(p) > 9:              # VOZ
                clave = (p[2:5].hex(), p[5])
                d = en_curso.setdefault(clave, {'ind': p[2:5].hex(), 'modo': p[7],
                                                'lotes': {}, 'rssi': 0})
                d['lotes'][p[6]] = p[9:]
            elif t == 0x83 and len(p) >= 4:             # FIN
                clave = (p[0:3].hex(), p[3])
                d = en_curso.pop(clave, None)
                if d and d['lotes']:
                    guarda(d, salida)
            elif t == 0x84 and len(p) >= 8:             # HOLA
                print('   baliza de %s (%d dBm)'
                      % (p[7:].decode('ascii', 'replace').strip(),
                         int.from_bytes(p[0:1], 'big', signed=True)))
    # lo que quedara a medias
    for d in en_curso.values():
        if d['lotes']:
            guarda(d, salida)


def guarda(d, base):
    nombre = POR_CODIGO.get(d['modo'], '1200')
    bits = b''.join(d['lotes'][k] for k in sorted(d['lotes']))
    open('/tmp/.rx.bin', 'wb').write(bits)
    wav = '%s-%s-%s.wav' % (base, d['ind'].replace('/', '_'),
                            time.strftime('%H%M%S'))
    try:
        subprocess.run(['c2dec', nombre, '/tmp/.rx.bin', '/tmp/.rx.raw'],
                       check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        subprocess.run(['sox', '-r', '8000', '-e', 'signed-integer', '-b', '16',
                        '-c', '1', '/tmp/.rx.raw', wav], check=True)
        segs = os.path.getsize('/tmp/.rx.raw') / 16000.0
        print('<< %s: %d lotes, %.1f s -> %s'
              % (d['ind'], len(d['lotes']), segs, wav))
    except Exception as e:
        print('<< no se pudo reconstruir:', e)


if __name__ == '__main__':
    main()
