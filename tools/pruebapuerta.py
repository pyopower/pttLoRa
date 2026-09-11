#!/usr/bin/env python3
"""Banco de la puerta del relevo de mando.

Levanta un `mandovirtual.py` de mentira en puertos altos y comprueba las cosas
que sostienen la puerta del 4464, que es el unico servicio de `or` que esta en
Internet y manda en un nodo entero:

  A. SIN SECRETO (como estuvo hasta el 11-sep-2026)
     1. un nodo que se anuncia entra, se le administra y los datos van y vienen
     2. a una conexion callada NO se le abre el socket de operador —por ahi van
        las claves del WiFi en claro— y se le quita la ranura al vencer la gracia
     3. con las cuatro ranuras llenas de conexiones calladas, el nodo de verdad
        las recupera en cuanto vencen

  B. CON SECRETO Y EXIGIENDOLO
     4. el que contesta bien al reto entra y se le administra
     5. el que contesta MAL se cierra en el acto, sin esperar a la gracia
     6. el que se identifica pero ignora el reto no llega al operador, y pierde
        la ranura al vencer la gracia

  C. CON SECRETO PERO SIN EXIGIRLO (el modo de un despliegue a medias)
     7. el que contesta mal SIGUE DENTRO y se le puede administrar — si no, al
        actualizar los nodos de uno en uno te quedas fuera de los que faltan

Tarda ~1 min: tres de las pruebas esperan a que venza GRACIA_S. No toca nada de
produccion; se ejecuta tal cual:  ./pruebapuerta.py
"""
import binascii, hashlib, hmac, os, socket, subprocess, sys, tempfile, time

RELEVO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mandovirtual.py")
P_NODOS, P_OP, RANURAS = 24464, 24471, 2
FEND, CMD_RETO, EV_RETO, EV_ESTADO = 0xC0, 0x19, 0x8B, 0x85
SECRETO = b"secreto-de-pruebas-no-es-el-de-verdad"
GRACIA = 16          # lo que espera el relevo + un segundo de margen

fallos = []


def comprueba(que, cond):
    print(("  OK   " if cond else "  FALLO") + "  " + que, flush=True)
    if not cond:
        fallos.append(que)


def estado(nombre):
    txt = "v1.52(lora32) EA1ABC nombre=%s perfil=auto secreto=si" % nombre
    return bytes([FEND, EV_ESTADO]) + txt.encode() + bytes([FEND])


def conecta(p, t=3):
    s = socket.create_connection(("127.0.0.1", p), t)
    s.settimeout(t)
    return s


def lee_reto(s):
    """Saca el reto de lo que manda el relevo nada mas aceptar."""
    s.settimeout(3)
    datos = b""
    t0 = time.time()
    while time.time() - t0 < 3:
        try:
            d = s.recv(4096)
        except socket.timeout:
            break
        if not d:
            break
        datos += d
        for t in datos.split(bytes([FEND])):
            if len(t) == 33 and t[0] == CMD_RETO:
                return t[1:]
    return b""


def contesta(reto, bien=True):
    sec = SECRETO if bien else b"otro-secreto-cualquiera"
    r = hmac.new(sec, reto, hashlib.sha256).hexdigest()[:32].encode()
    return bytes([FEND, EV_RETO]) + r + bytes([FEND])


def muerto(s):
    """True si el otro extremo ha cerrado. Hay que VACIAR lo pendiente (el reto,
    la peticion de estado) antes de mirarlo; si no, se lee eso y parece vivo."""
    s.settimeout(3)
    try:
        while True:
            if s.recv(4096) == b"":
                return True
    except OSError:
        return False


def arranca(*extra):
    p = subprocess.Popen([sys.executable, RELEVO, "--nodos", str(P_NODOS),
                          "--operador", str(P_OP), "--ranuras", str(RANURAS)]
                         + list(extra),
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(1.5)
    return p


def para(p):
    p.terminate()
    try:
        return p.communicate(timeout=5)[0]
    except subprocess.TimeoutExpired:
        p.kill()
        return p.communicate()[0]


def op_abierto(p=P_OP):
    """El relevo cierra en el acto el socket del operador que no toca abrir."""
    try:
        s = conecta(p)
        s.settimeout(2)
        try:
            vivo = s.recv(16) != b""
        except socket.timeout:
            vivo = True          # abierto y a la espera: correcto
        s.close()
        return vivo
    except OSError:
        return False


registros = []

# ---------------------------------------------------------------- A, sin secreto
print("\nA. SIN SECRETO")
rel = arranca()
try:
    print(" 1) nodo de verdad: entra, se anuncia, se le administra")
    nodo = conecta(P_NODOS)
    time.sleep(0.4)
    comprueba("le piden el estado al entrar", nodo.recv(64) == bytes([FEND, 0x05, FEND]))
    nodo.sendall(estado("celdaPRUEBA"))
    time.sleep(0.6)
    op = conecta(P_OP)
    op.sendall(b"\xc0\x05\xc0")
    time.sleep(0.4)
    comprueba("el operador llega al nodo", nodo.recv(64) == b"\xc0\x05\xc0")
    nodo.sendall(estado("celdaPRUEBA"))
    time.sleep(0.4)
    comprueba("y el nodo llega al operador", b"celdaPRUEBA" in op.recv(4096))
    op.close(); nodo.close(); time.sleep(0.5)

    print(" 2) escaner callado: ni administra ni se queda con la ranura")
    esc = conecta(P_NODOS)
    time.sleep(0.5)
    comprueba("no se abre el operador contra algo sin identificar", not op_abierto())
    print("     (esperando la gracia, %d s...)" % GRACIA, flush=True)
    time.sleep(GRACIA)
    comprueba("al escaner se le echa al vencer la gracia", muerto(esc))
    esc.close()

    print(" 3) ranuras llenas de escaneres: el nodo las recupera")
    okupas = [conecta(P_NODOS) for _ in range(RANURAS)]
    time.sleep(0.5)
    try:
        sobra = conecta(P_NODOS); sobra.settimeout(2)
        rechazado = sobra.recv(16) == b""
        sobra.close()
    except OSError:
        rechazado = True
    comprueba("con todo lleno, al de mas se le rechaza (esperado)", rechazado)
    print("     (esperando la gracia, %d s...)" % GRACIA, flush=True)
    time.sleep(GRACIA)
    tarde = conecta(P_NODOS)
    time.sleep(0.4)
    tarde.sendall(estado("celdaPRUEBA"))
    time.sleep(0.6)
    comprueba("el nodo recupera ranura y se le puede administrar", op_abierto())
    for s in okupas + [tarde]:
        try: s.close()
        except OSError: pass
finally:
    registros.append(para(rel))

# ------------------------------------------------------- B, con secreto y exigido
with tempfile.NamedTemporaryFile("wb", suffix=".secreto", delete=False) as f:
    f.write(SECRETO)
    fsec = f.name
os.chmod(fsec, 0o600)

print("\nB. CON SECRETO, EXIGIDO")
rel = arranca("--secreto", fsec, "--exige")
try:
    print(" 4) el que contesta bien entra")
    nodo = conecta(P_NODOS)
    reto = lee_reto(nodo)
    comprueba("el relevo manda un reto de 32 hex", len(reto) == 32)
    nodo.sendall(contesta(reto) + estado("celdaPRUEBA"))
    time.sleep(0.8)
    comprueba("con el reto bien, se le administra", op_abierto())
    nodo.close(); time.sleep(0.5)

    print(" 5) el que contesta mal se cierra en el acto")
    malo = conecta(P_NODOS)
    reto = lee_reto(malo)
    malo.sendall(contesta(reto, bien=False) + estado("celdaFALSA"))
    time.sleep(1.0)
    comprueba("al que contesta mal se le cierra sin esperar la gracia", muerto(malo))
    malo.close(); time.sleep(0.5)

    print(" 6) el que ignora el reto: identificado, pero no manda")
    mudo = conecta(P_NODOS)
    lee_reto(mudo)
    mudo.sendall(estado("celdaPRUEBA"))       # se identifica, pero no contesta
    time.sleep(0.8)
    comprueba("identificado sin reto: NO se abre el operador", not op_abierto())
    print("     (esperando la gracia, %d s...)" % GRACIA, flush=True)
    time.sleep(GRACIA)
    comprueba("y pierde la ranura al vencer la gracia", muerto(mudo))
    mudo.close()
finally:
    registros.append(para(rel))

# ------------------------------------------- C, con secreto pero sin exigirlo aun
print("\nC. CON SECRETO, SOLO AVISANDO (despliegue a medias)")
rel = arranca("--secreto", fsec)
try:
    print(" 7) el que aun no sabe el secreto sigue siendo administrable")
    viejo = conecta(P_NODOS)
    reto = lee_reto(viejo)
    viejo.sendall(contesta(reto, bien=False) + estado("celdaVIEJA"))
    time.sleep(1.0)
    comprueba("sin --exige, el que contesta mal sigue dentro", not muerto(viejo))
    comprueba("sin --exige, se le puede administrar igual", op_abierto())
    viejo.close()
finally:
    registros.append(para(rel))
    os.unlink(fsec)

for r in registros:
    print("\n--- registro del relevo ---")
    print(r)
print("RESULTADO:", "TODO OK" if not fallos else "FALLOS: %s" % fallos)
sys.exit(1 if fallos else 0)
