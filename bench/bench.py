#!/usr/bin/env python3
# bench.py — banco de pruebas de codec para el canal de voz por LoRa.
#
#   ./bench.py <entrada.wav|entrada.raw> [salida/]
#
# Pasa la misma locucion por varios modos de Codec2 y deja un .wav de cada uno
# para poder juzgarlos de oido. Ademas simula lo que de verdad pasa en LoRa: no
# hay ruido progresivo, hay **paquetes que no llegan**. Un lote perdido son
# 480 ms de silencio, y eso destroza la inteligibilidad mucho mas que el ruido
# de cuantificacion del codec.
#
# Por eso se comparan dos estrategias con el MISMO gasto de aire:
#   - 1200 mandado una vez
#   - 700C mandado dos veces, intercalado (un lote solo se pierde si caen las
#     dos copias, y las copias van separadas en el tiempo)
#
# La entrada tiene que ser voz de verdad para que el juicio valga: un TTS pasa
# por el modelo LPC del codec de una forma que no representa una voz humana.
# Para grabarte: cualquier grabadora del movil, luego
#   sox grabacion.m4a -r 8000 -c 1 -b 16 -e signed-integer voz.raw

import os, random, subprocess, sys, wave

FS = 8000
LOTE_MS = 480                      # audio por paquete de LoRa
MODOS = ['3200', '2400', '1600', '1300', '1200', '700C', '450']
PERDIDAS = [0.0, 0.10, 0.25]       # fraccion de paquetes que no llegan


def a_raw(entrada, destino):
    if entrada.endswith('.raw'):
        return entrada
    subprocess.run(['sox', entrada, '-r', str(FS), '-c', '1', '-b', '16',
                    '-e', 'signed-integer', destino], check=True)
    return destino


def codec(modo, raw_in, tmp):
    bits = os.path.join(tmp, 'c.bin')
    out = os.path.join(tmp, 'c.raw')
    subprocess.run(['c2enc', modo, raw_in, bits], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(['c2dec', modo, bits, out], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return open(out, 'rb').read(), os.path.getsize(bits)


def perder(pcm, p, doble=False, semilla=1):
    """Borra lotes enteros de 480 ms, que es como se pierde en LoRa.

    Las dos estrategias sufren EL MISMO canal: la primera copia usa la misma
    tirada de dados en los dos casos (misma semilla), y la segunda copia solo
    aparece en el modo doble. Asi los huecos del doble envio son siempre un
    subconjunto de los del envio simple, que es lo que pasaria de verdad.
    """
    if p <= 0:
        return pcm, 0
    r1 = random.Random(semilla)          # primera copia: identica en ambos casos
    r2 = random.Random(semilla + 1000)   # segunda copia, separada en el tiempo
    n = FS * LOTE_MS // 1000 * 2                     # bytes por lote
    fuera = 0
    trozos = []
    for i in range(0, len(pcm), n):
        t = pcm[i:i + n]
        cae = r1.random() < p
        if doble:
            cae = cae and r2.random() < p
        if cae:
            t = b'\x00' * len(t)
            fuera += 1
        trozos.append(t)
    return b''.join(trozos), fuera


def guardar(path, pcm):
    with wave.open(path, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(FS)
        w.writeframes(pcm)


def main():
    entrada = sys.argv[1]
    salida = sys.argv[2] if len(sys.argv) > 2 else 'salida'
    os.makedirs(salida, exist_ok=True)
    tmp = os.path.join(salida, '.tmp')
    os.makedirs(tmp, exist_ok=True)
    raw = a_raw(entrada, os.path.join(tmp, 'in.raw'))
    dur = os.path.getsize(raw) / (FS * 2)
    print('entrada: %.1f s de audio a %d Hz\n' % (dur, FS))

    guardar(os.path.join(salida, '00-original.wav'), open(raw, 'rb').read())

    # OJO: c2enc rellena hasta byte entero por trama, asi que 700C sale a 800
    # bps y no a 700. En el protocolo real conviene empaquetar los bits de las
    # 12 tramas del lote seguidos y se recupera ese 12%.
    print('%-8s %-9s %-9s %s' % ('modo', 'bits/s', 'B por lote', 'fichero'))
    cache = {}
    for m in MODOS:
        pcm, nbits = codec(m, raw, tmp)
        cache[m] = pcm
        bps = nbits * 8 / dur
        guardar(os.path.join(salida, '01-codec2-%s.wav' % m), pcm)
        print('%-8s %-9.0f %-9.0f 01-codec2-%s.wav'
              % (m, bps, bps * LOTE_MS / 1000 / 8, m))

    print('\nCon paquetes perdidos (lotes de %d ms):' % LOTE_MS)
    print('%-28s %-9s %s' % ('estrategia', 'perdida', 'fichero'))
    for p in PERDIDAS[1:]:
        a, na = perder(cache['1200'], p)
        b, nb = perder(cache['700C'], p, doble=True)
        f1 = '02-perdida%02d-1200-simple.wav' % (p * 100)
        f2 = '02-perdida%02d-700C-doble.wav' % (p * 100)
        guardar(os.path.join(salida, f1), a)
        guardar(os.path.join(salida, f2), b)
        print('%-28s %-9s %s  (%d huecos)' % ('1200 x1', '%d%%' % (p * 100), f1, na))
        print('%-28s %-9s %s  (%d huecos)' % ('700C x2 intercalado',
                                              '%d%%' % (p * 100), f2, nb))
    print('\nTodo en %s/' % salida)


if __name__ == '__main__':
    main()
