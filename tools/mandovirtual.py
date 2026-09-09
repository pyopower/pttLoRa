#!/usr/bin/env python3
"""Relevo de mando para nodos PTT LoRa que no pueden abrir puertos.

El problema: un nodo instalado en un sitio prestado (un tejado, la red de un
tercero) esta en una red donde no se pueden abrir puertos entrantes. Y todo lo
que sirve para administrarlo va HACIA DENTRO: el estado y la configuracion
(4460) y la OTA de Arduino (3232). Instalado alli, ese nodo queda incomunicado
salvo bajandolo.

La vuelta: **que llame el**. El nodo abre una conexion saliente contra este
relevo (`nodo.py mando <host> <puerto>`) y se comporta como si por ahi hubiera
entrado un cliente normal. Aqui se le empareja con quien quiera administrarlo.

    ./mandovirtual.py [--nodos 4464] [--operador 4471] [--ranuras 4]

  - Los nodos entran por 4464 (abierto a Internet).
  - Cada nodo cae en una ranura y se le atiende en 4471+ranura, **solo desde
    127.0.0.1**: quien pueda hablar por ahi manda en el nodo entero, asi que no
    se asoma a Internet. Se llega con un tunel:
        ssh -L 4471:127.0.0.1:4471 or.adan.ovh
        ./nodo.py --tcp 127.0.0.1:4471 estado

Detalle que no es opcional: **el nodo suelta a un cliente que lleva 3 minutos
callado**. Por eso el relevo le pregunta el estado cada minuto mientras no haya
nadie atendiendole; ademas de mantener el tubo, asi se sabe quien esta en cada
ranura sin tener que conectarse a mirar.
"""
import selectors, socket, sys, threading, time

FEND, CMD_ESTADO, EV_ESTADO = 0xC0, 0x05, 0x85
LATIDO_S = 60


def opt(a, nombre, tipo, defecto):
    if nombre in a:
        i = a.index(nombre)
        v = tipo(a[i + 1])
        del a[i:i + 2]
        return v
    return defecto


class Ranura:
    def __init__(self, n):
        self.n = n
        self.nodo = None          # socket del nodo
        self.op = None            # socket de quien lo administra
        self.quien = "?"          # indicativo, en cuanto se sepa
        self.desde = 0
        self.ultimo_latido = 0
        self.resto = b""

    def dice(self, *a):
        print("[%s] ranura %d:" % (time.strftime("%H:%M:%S"), self.n), *a, flush=True)

    def cierra_nodo(self):
        if self.nodo:
            try: self.nodo.close()
            except OSError: pass
        self.nodo = None
        self.quien = "?"
        self.resto = b""

    def cierra_op(self):
        if self.op:
            try: self.op.close()
            except OSError: pass
        self.op = None


def mira_estado(r, datos):
    """Se queda con el indicativo que anuncia el nodo, sin estorbar al operador."""
    r.resto = (r.resto + datos)[-512:]
    trozos = r.resto.split(bytes([FEND]))
    for t in trozos:
        if len(t) > 2 and t[0] == EV_ESTADO:
            txt = t[1:].decode("ascii", "replace")
            campos = txt.split(" ")
            if len(campos) > 1 and campos[0].startswith("v"):
                if r.quien != campos[1]:
                    r.quien = campos[1]
                    r.dice("es", r.quien, "-", " ".join(campos[:2]))


def main():
    a = sys.argv[1:]
    p_nodos = opt(a, "--nodos", int, 4464)
    p_op = opt(a, "--operador", int, 4471)
    n_ran = opt(a, "--ranuras", int, 4)

    ranuras = [Ranura(i) for i in range(n_ran)]
    sel = selectors.DefaultSelector()

    s_nodos = socket.socket()
    s_nodos.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s_nodos.bind(("", p_nodos)); s_nodos.listen(8)
    sel.register(s_nodos, selectors.EVENT_READ, ("nodos", None))

    for r in ranuras:
        s = socket.socket()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        # SOLO local: por aqui se manda en el nodo entero.
        s.bind(("127.0.0.1", p_op + r.n)); s.listen(1)
        sel.register(s, selectors.EVENT_READ, ("op", r))

    print("-- relevo de mando: nodos en %d, operadores en %d-%d (solo 127.0.0.1)"
          % (p_nodos, p_op, p_op + n_ran - 1), flush=True)

    while True:
        for clave, _ in sel.select(timeout=1.0):
            tipo, r = clave.data
            if tipo == "nodos":
                c, dir_ = clave.fileobj.accept()
                libre = next((x for x in ranuras if x.nodo is None), None)
                if libre is None:
                    print("-- sin ranuras libres; se rechaza", dir_[0], flush=True)
                    c.close()
                    continue
                c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                libre.nodo = c
                libre.desde = time.time()
                libre.ultimo_latido = 0
                sel.register(c, selectors.EVENT_READ, ("nodo", libre))
                libre.dice("nodo desde", dir_[0], "-> atiendelo en el %d" % (p_op + libre.n))

            elif tipo == "op":
                c, _ = clave.fileobj.accept()
                if r.nodo is None:
                    c.close()
                    continue
                if r.op:
                    r.dice("ya habia un operador; se le releva")
                    sel.unregister(r.op); r.cierra_op()
                c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                r.op = c
                sel.register(c, selectors.EVENT_READ, ("opdatos", r))
                r.dice("operador dentro (%s)" % r.quien)

            elif tipo == "nodo":
                try: d = r.nodo.recv(4096)
                except OSError: d = b""
                if not d:
                    r.dice("el nodo se ha ido")
                    sel.unregister(r.nodo); r.cierra_nodo()
                    if r.op:
                        sel.unregister(r.op); r.cierra_op()
                    continue
                mira_estado(r, d)
                if r.op:
                    try: r.op.sendall(d)
                    except OSError:
                        sel.unregister(r.op); r.cierra_op()

            elif tipo == "opdatos":
                try: d = r.op.recv(4096)
                except OSError: d = b""
                if not d:
                    r.dice("operador fuera")
                    sel.unregister(r.op); r.cierra_op()
                    continue
                if r.nodo:
                    try: r.nodo.sendall(d)
                    except OSError:
                        sel.unregister(r.nodo); r.cierra_nodo()

        # Latido: sin esto el nodo suelta el tubo a los 3 minutos de silencio.
        ahora = time.time()
        for r in ranuras:
            if r.nodo and not r.op and ahora - r.ultimo_latido > LATIDO_S:
                r.ultimo_latido = ahora
                try:
                    r.nodo.sendall(bytes([FEND, CMD_ESTADO, FEND]))
                except OSError:
                    r.dice("se corto al latir")
                    sel.unregister(r.nodo); r.cierra_nodo()


if __name__ == "__main__":
    sys.exit(main())
