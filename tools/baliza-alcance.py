#!/usr/bin/env python3
"""Baliza de voz para medir alcance, sin tocar el firmware.

El nodo NO sabe hablar: el codec vive en los extremos a proposito, asi que la
placa solo mueve bytes. Esta baliza hace de "movil fijo": se cuelga del nodo
por WiFi (o por USB) y le mete voz de verdad cada pocos segundos, para poder
irse con el otro nodo lejos y escuchar hasta donde llega.

    ./baliza-alcance.py --tcp 192.168.1.61 --ident C31AG-9
    ./baliza-alcance.py --tcp 192.168.1.61 --segundos 10 --pausa 5

Cada transmision lleva un NUMERO cantado: al volver, se sabe exactamente
cuantas se perdieron y en que punto del paseo, que es justo lo que hay que
medir. El texto va con espeak y se convierte a 8 kHz con sox.

Se reconecta sola: en una prueba de campo el nodo puede reiniciarse o perder
el WiFi, y volver a casa a arrancar la baliza no es plan.
"""
import os, subprocess, sys, time

AQUI = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, AQUI)
import importlib.util
spec = importlib.util.spec_from_file_location("nodo", os.path.join(AQUI, "nodo.py"))
nodo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nodo)


def opt(a, nombre, tipo, defecto):
    if nombre in a:
        i = a.index(nombre)
        v = tipo(a[i + 1])
        del a[i:i + 2]
        return v
    return defecto


def voz(texto, segundos, tmp="/tmp/.pttlora_baliza"):
    """Texto -> PCM 8 kHz mono de EXACTAMENTE `segundos`."""
    subprocess.run(["espeak", "-v", "es", "-s", "150", "-w", tmp + ".wav", texto],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["sox", tmp + ".wav", "-r", "8000", "-c", "1", "-b", "16",
                    "-e", "signed-integer", tmp + ".raw"], check=True)
    raw = open(tmp + ".raw", "rb").read()
    quiero = int(8000 * segundos) * 2
    if len(raw) > quiero:
        raw = raw[:quiero]                      # cortado
    else:
        raw += b"\x00" * (quiero - len(raw))    # relleno con silencio
    return raw


def main():
    a = sys.argv[1:]
    tcp = opt(a, "--tcp", str, "")
    puerto = opt(a, "--puerto", str, "")
    ident = opt(a, "--ident", str, "C31AG-9")
    segundos = opt(a, "--segundos", float, 10.0)
    pausa = opt(a, "--pausa", float, 5.0)
    modo = opt(a, "--modo", str, "1200")
    if not tcp and not puerto:
        print("hace falta --tcp <ip> o --puerto <serie>")
        return 1

    print("-- baliza %s: %.0f s de voz cada %.0f s de pausa, codec %s"
          % (ident, segundos, pausa, modo), flush=True)
    n = None
    numero = 0
    while True:
        try:
            if n is None:
                n = nodo.Nodo(tcp=tcp) if tcp else nodo.Nodo(puerto)
                n.manda(nodo.CMD_IDENT, ident.encode("ascii"))
                print("-- enlazado con el nodo", flush=True)
            numero += 1
            texto = ("Baliza de alcance de %s. Transmision numero %d. "
                     "Uno, dos, tres, cuatro, cinco. Fin de la %d."
                     % (ident.replace("-", " "), numero, numero))
            raw = voz(texto, segundos)
            cod, n_tramas, lotes = nodo.codifica(raw, modo)
            n.manda(nodo.CMD_INICIO, bytes([cod]))
            t0 = time.time()
            for i, lote in enumerate(lotes):
                n.manda(nodo.CMD_VOZ, bytes([cod, n_tramas]) + lote)
                espera = t0 + (i + 1) * nodo.LOTE_MS / 1000.0 - time.time()
                if espera > 0:
                    time.sleep(espera)
            n.manda(nodo.CMD_FIN)
            print("-- %s transmision %d: %d lotes" % (time.strftime("%H:%M:%S"),
                                                      numero, len(lotes)), flush=True)
            time.sleep(pausa)
        except KeyboardInterrupt:
            print("-- baliza parada")
            return 0
        except Exception as e:
            print("-- se corto (%s: %s); reintento en 5 s"
                  % (type(e).__name__, e), flush=True)
            try:
                n.s.close()
            except Exception:
                pass
            n = None
            time.sleep(5)


if __name__ == "__main__":
    sys.exit(main())
