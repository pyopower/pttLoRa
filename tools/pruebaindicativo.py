#!/usr/bin/env python3
"""Banco del filtro de indicativo del nodo de datos (4460).

Lo que entra por el 4460 acaba SALIENDO POR LA ANTENA de una celda, con el
indicativo que diga el que habla. Esto comprueba que con `--exige-indicativo`:

  1. quien no se identifica NO transmite (por defecto era `ANON`, y emitia)
  2. quien se identifica con basura (`AAAAA`) tampoco
  3. quien da un indicativo con forma de indicativo SI
  4. y el que no puede transmitir SIGUE OYENDO — oir no hace emitir a nadie
  5. sin el flag, todo pasa como siempre (la vuelta atras funciona)

Y antes de nada, la TABLA: indicativos de medio mundo contra basura. Es la
parte que de verdad evita el desastre, que no es dejar pasar a un listo — es
CALLAR A ALGUIEN QUE TIENE LICENCIA porque su indicativo no se parece al tuyo.

Levanta un reflector y un nodo de datos de mentira en puertos altos; no toca
produccion.  ./pruebaindicativo.py
"""
import os, socket, subprocess, sys, time

AQUI = os.path.dirname(os.path.abspath(__file__))
P_REF, P_DAT = 24461, 24460
FEND = 0xC0
CMD_INICIO, CMD_VOZ, CMD_FIN, CMD_IDENT = 0x01, 0x02, 0x03, 0x0E
EV_INICIO, EV_VOZ = 0x81, 0x82

fallos = []

# Indicativos que TIENEN que poder hablar. Los cuatro primeros son los que se
# usan de verdad en produccion; el resto cubre las formas raras pero legitimas
# de la UIT y lo que la app y el uso pegan detras.
BUENOS = [
    'C31AG', 'C31AG-2', 'C31AG HONOR', 'C31AG-7 Adan',
    'C31AG-1', 'C31AG-01', 'C31AG-55', 'C31AG-99',   # varios cacharros, -1..-99
    'EA3ABC', 'EA3ABC-9', 'ea3abc-12',               # minusculas
    'EA3ABC/P', 'F/EA3ABC',                          # portable y otro pais
    'W1AW', '2E0ABC', '9A1A', '4X4ABC', '3DA0RS',    # prefijos con digito
]
# Y los que no.
MALOS = [
    'ANON', 'AAAAA', '12345', 'NOCALL', 'N0CALL', 'TEST',
    '', '   ', '1234A', 'ABCDEFG', '999', 'EA3ABCDEF',
    'C31AG-0', 'C31AG-00', 'C31AG-100', 'C31AG-', 'C31AG-A', 'C31AG-55-3',
]


def comprueba(que, cond):
    print(("  OK   " if cond else "  FALLO") + "  " + que, flush=True)
    if not cond:
        fallos.append(que)


def marco(tipo, datos=b''):
    out = bytearray([FEND, tipo])
    for b in datos:                      # escape KISS
        if b == FEND:   out += bytes([0xDB, 0xDC])
        elif b == 0xDB: out += bytes([0xDB, 0xDD])
        else:           out.append(b)
    out.append(FEND)
    return bytes(out)


def cliente(ind=None):
    s = socket.create_connection(("127.0.0.1", P_DAT), 3)
    s.settimeout(2)
    if ind is not None:
        s.sendall(marco(CMD_IDENT, ind.encode()))
        time.sleep(0.3)
    return s


def habla(s):
    s.sendall(marco(CMD_INICIO, bytes([4])))
    s.sendall(marco(CMD_VOZ, bytes([4, 1]) + b'x' * 48))
    s.sendall(marco(CMD_FIN))
    time.sleep(0.6)


def ha_oido(obs):
    """¿Le ha llegado al observador algun INICIO o VOZ?"""
    obs.settimeout(1.5)
    datos = b''
    try:
        while True:
            d = obs.recv(4096)
            if not d:
                break
            datos += d
    except socket.timeout:
        pass
    return any(t and t[0] in (EV_INICIO, EV_VOZ) for t in datos.split(bytes([FEND])))


def arranca(*extra):
    ref = subprocess.Popen([sys.executable, os.path.join(AQUI, "nodovirtual.py"),
                            "--escucha", str(P_REF)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.0)
    dat = subprocess.Popen([sys.executable, os.path.join(AQUI, "nododatos.py"),
                            "--escucha", str(P_DAT),
                            "--reflector", "127.0.0.1:%d" % P_REF] + list(extra),
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(1.5)
    return ref, dat


def para(ref, dat):
    dat.terminate(); ref.terminate()
    try:    salida = dat.communicate(timeout=5)[0]
    except subprocess.TimeoutExpired:
        dat.kill(); salida = dat.communicate()[0]
    try:    ref.wait(timeout=5)
    except subprocess.TimeoutExpired:
        ref.kill()
    return salida


print("\nLA TABLA: %d indicativos buenos, %d malos" % (len(BUENOS), len(MALOS)))
sys.path.insert(0, AQUI)
from nododatos import parece_indicativo
_mal = [c for c in BUENOS if not parece_indicativo(c)]
comprueba("no calla a nadie con licencia%s" % (" — RECHAZA %s" % _mal if _mal else ""),
          not _mal)
_mal = [c for c in MALOS if parece_indicativo(c)]
comprueba("no deja emitir a la basura%s" % (" — ACEPTA %s" % _mal if _mal else ""),
          not _mal)

print("\nCON --exige-indicativo")
ref, dat = arranca("--exige-indicativo")
try:
    obs = cliente("EA9ZZZ")                      # el que escucha, siempre el mismo
    time.sleep(0.3); ha_oido(obs)                # vaciar lo que hubiera

    mudo = cliente(None); habla(mudo)
    comprueba("el que no se identifica NO sale", not ha_oido(obs))
    mudo.close()

    basura = cliente("AAAAA"); habla(basura)
    comprueba("el que dice AAAAA NO sale", not ha_oido(obs))
    basura.close()

    bueno = cliente("EA3ABC"); habla(bueno)
    comprueba("el que da un indicativo SI sale", ha_oido(obs))
    bueno.close()

    # Y al reves: un cliente con indicativo de basura tiene que SEGUIR OYENDO.
    sordo = cliente("12345")
    time.sleep(0.3); ha_oido(sordo)
    otro = cliente("EA3ABC"); habla(otro)
    comprueba("el que no puede transmitir SIGUE oyendo", ha_oido(sordo))
    otro.close(); sordo.close(); obs.close()
finally:
    reg1 = para(ref, dat)

print("\nSIN el flag (la vuelta atras)")
ref, dat = arranca()
try:
    obs = cliente("EA9ZZZ")
    time.sleep(0.3); ha_oido(obs)
    mudo = cliente(None); habla(mudo)
    comprueba("sin exigir, hasta el que no se identifica sale", ha_oido(obs))
    mudo.close(); obs.close()
finally:
    reg2 = para(ref, dat)

for r in (reg1, reg2):
    print("\n--- registro del nodo de datos ---")
    print(r)
print("RESULTADO:", "TODO OK" if not fallos else "FALLOS: %s" % fallos)
sys.exit(1 if fallos else 0)
