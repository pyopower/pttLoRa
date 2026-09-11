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
                      [--secreto <fichero>] [--exige]

  - Los nodos entran por 4464 (abierto a Internet).
  - Cada nodo cae en una ranura y se le atiende en 4471+ranura, **solo desde
    127.0.0.1**: quien pueda hablar por ahi manda en el nodo entero, asi que no
    se asoma a Internet. Se llega con un tunel:
        ssh -L 4471:127.0.0.1:4471 or.adan.ovh
        ./nodo.py --tcp 127.0.0.1:4471 estado

La puerta de los nodos (4464) esta en Internet y no pide credenciales, asi que
la entrada la gobierna una regla sencilla: **el que no dice quien es no se
queda**. Nada mas entrar se le pide el estado; si en GRACIA_S segundos no ha
mandado un EV_ESTADO valido, se le cierra y la ranura vuelve a estar libre. Y a
una ranura sin identificar **no se le abre el socket de operador**, porque por
ahi viajan las claves del WiFi en claro. Esto no es autenticacion —quien sepa
hablar el protocolo entra igual—, es lo que hace falta para que un barrido de
puertos no deje a un nodo de tejado incomunicado (paso el 10-sep: 124
conexiones de un EC2 de Oregon).

Detalle que no es opcional: **el nodo suelta a un cliente que lleva 3 minutos
callado**. Por eso el relevo le pregunta el estado cada minuto mientras no haya
nadie atendiendole; ademas de mantener el tubo, asi se sabe quien esta en cada
ranura sin tener que conectarse a mirar.
"""
import binascii, hashlib, hmac, os, selectors, socket, sys, threading, time

FEND, CMD_ESTADO, EV_ESTADO = 0xC0, 0x05, 0x85
CMD_RETO, EV_RETO = 0x19, 0x8B
LATIDO_S = 60
# Lo que se espera a que una conexion diga quien es antes de echarla. Un nodo
# de verdad se anuncia SOLO al entrar —en el registro sale en el mismo segundo
# que el "nodo desde"—, asi que 15 s es holgadisimo y aun asi corta en seco al
# que llama y se queda callado.
GRACIA_S = 15

# El secreto compartido con los nodos. Se lee de un fichero, NUNCA se escribe en
# el registro ni viaja por el cable: lo que va y viene es un reto al azar y su
# HMAC. Vacio = no se reta a nadie (asi arranca quien clone el proyecto).
SECRETO = b""
EXIGE = False


def responde_al_reto(reto):
    """Lo que tiene que contestar un nodo con el secreto. Los 16 primeros
    bytes del HMAC-SHA256 del reto, en hex; el firmware hace exactamente esto
    (ver CMD_RETO en protocolo.h)."""
    return hmac.new(SECRETO, reto, hashlib.sha256).hexdigest()[:32].encode()


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
        # El NOMBRE es lo que distingue a un nodo de otro: todos los nodos de
        # un operador llevan el mismo indicativo, asi que `quien` no sirve para
        # reconocer a uno que vuelve. Ver `mira_estado`.
        self.nombre = "?"
        self.desde = 0
        self.ultimo_latido = 0
        self.resto = b""
        self.reto = b""        # lo que se le mando; cambia en cada conexion
        self.auth = False      # ha contestado bien al reto
        self.avisado = False   # ya se dijo por el registro que no contestaba

    def dice(self, *a):
        print("[%s] ranura %d:" % (time.strftime("%H:%M:%S"), self.n), *a, flush=True)

    def cierra_nodo(self):
        if self.nodo:
            try: self.nodo.close()
            except OSError: pass
        self.nodo = None
        self.quien = "?"
        self.nombre = "?"
        self.resto = b""
        self.reto = b""
        self.auth = False
        self.avisado = False

    def cierra_op(self):
        if self.op:
            try: self.op.close()
            except OSError: pass
        self.op = None


def mira_estado(r, datos, ranuras=(), sel=None):
    """Se queda con el indicativo y el NOMBRE que anuncia el nodo, sin estorbar
    al operador.

    Y de paso echa a su propio fantasma. Cuando un nodo se reinicia —una OTA,
    un corte de luz— la conexion vieja NO se cierra: el ESP32 se va sin decir
    adios y el socket muerto sigue ocupando su ranura hasta que un envio falle.
    Asi que el nodo que vuelve cae en OTRA ranura, el tunel de siempre apunta a
    un tubo muerto (parece que la OTA lo ha matado) y, sobre todo, **las cuatro
    ranuras se van llenando de fantasmas del mismo nodo**: a la cuarta vez se
    rechaza la conexion y el nodo queda INALCANZABLE. Para uno que esta en un
    tejado ajeno eso no tiene arreglo remoto.

    En cuanto se identifica, cualquier otra ranura con el mismo nombre es el
    fantasma de este mismo nodo, y se cierra."""
    r.resto += datos
    # Red de seguridad: si por lo que sea no aparece un FEND, esto no puede
    # crecer sin fin. Pero el corte NO puede ser la forma normal de vaciarlo:
    # con un tope de 512 bytes se perdia el principio de la trama —y con el, el
    # byte EV_ESTADO que la identifica—, porque la linea de estado pasa de 600
    # caracteres. Resultado: `quien` se quedaba en "?" PARA SIEMPRE y el log
    # decia "operador dentro (?)" en cada entrada. Un fallo mudo de meses.
    if len(r.resto) > 8192:
        r.resto = r.resto[-8192:]
    trozos = r.resto.split(bytes([FEND]))
    # Lo ultimo es lo que aun no ha terminado: se guarda para la proxima vuelta
    # y lo demas son tramas completas, cada una vista UNA vez.
    r.resto = trozos[-1]
    for t in trozos[:-1]:
        if len(t) > 2 and t[0] == EV_RETO and SECRETO and r.reto and not r.auth:
            # compare_digest y no ==: el tiempo que tarda un == en fallar dice
            # por donde se parecian las cadenas, y eso se puede medir.
            if hmac.compare_digest(t[1:33], responde_al_reto(r.reto)):
                r.auth = True
                r.dice("contesta bien al reto")
            else:
                r.dice("CONTESTA MAL AL RETO" +
                       ("; se cierra" if EXIGE else " (no se exige: se le deja)"))
                if EXIGE:
                    if sel is not None:
                        try: sel.unregister(r.nodo)
                        except (KeyError, ValueError): pass
                    r.cierra_nodo()
                    if r.op:
                        if sel is not None:
                            try: sel.unregister(r.op)
                            except (KeyError, ValueError): pass
                        r.cierra_op()
                    return
        if len(t) > 2 and t[0] == EV_ESTADO:
            txt = t[1:].decode("ascii", "replace")
            campos = txt.split(" ")
            if len(campos) > 1 and campos[0].startswith("v"):
                if r.quien != campos[1]:
                    r.quien = campos[1]
                    r.dice("es", r.quien, "-", " ".join(campos[:2]))
                nom = "?"
                for c in campos:
                    if c.startswith("nombre="):
                        nom = c[7:]
                        break
                if nom != "?" and nom != r.nombre:
                    r.nombre = nom
                    r.dice("se llama", nom)
                    for otra in ranuras:
                        if otra is r or otra.nodo is None:
                            continue
                        if otra.nombre == nom:
                            otra.dice("es el fantasma de", nom,
                                      "- ha vuelto en la %d; se cierra" % r.n)
                            if sel is not None:
                                try: sel.unregister(otra.nodo)
                                except (KeyError, ValueError): pass
                            otra.cierra_nodo()
                            if otra.op:
                                if sel is not None:
                                    try: sel.unregister(otra.op)
                                    except (KeyError, ValueError): pass
                                otra.cierra_op()


def main():
    a = sys.argv[1:]
    global SECRETO, EXIGE
    p_nodos = opt(a, "--nodos", int, 4464)
    p_op = opt(a, "--operador", int, 4471)
    n_ran = opt(a, "--ranuras", int, 4)
    fich = opt(a, "--secreto", str, "")
    EXIGE = "--exige" in a
    if fich:
        with open(fich, "rb") as f:
            SECRETO = f.read().strip()
    if EXIGE and not SECRETO:
        print("-- se pide --exige sin secreto: no hay nada que exigir", flush=True)
        return 2

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
    print("-- reto: %s" % ("no hay secreto; entra quien sepa el protocolo"
                           if not SECRETO else
                           "EXIGIDO; sin contestar bien no hay ranura" if EXIGE else
                           "solo se avisa (aun no se exige)"), flush=True)

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
                # KEEPALIVE, para que un fantasma se caiga sin esperar a que
                # falle un envio: el latido escribe en un socket muerto sin
                # error durante mucho tiempo. 30 s de silencio y tres sondeos
                # cada 10 -> el fantasma cae en un minuto largo, no en horas.
                try:
                    c.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
                    c.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE, 30)
                    c.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL, 10)
                    c.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT, 3)
                except (AttributeError, OSError):
                    pass
                libre.nodo = c
                libre.desde = time.time()
                # El estado se le pide EN EL ACTO, no al cabo de un minuto: es
                # lo que le da pie a identificarse antes de que venza la
                # gracia. Si no contesta, la ranura se le quita.
                libre.ultimo_latido = libre.desde
                sel.register(c, selectors.EVENT_READ, ("nodo", libre))
                libre.dice("nodo desde", dir_[0], "-> atiendelo en el %d" % (p_op + libre.n))
                try:
                    if SECRETO:
                        # 16 bytes al azar en hex. En hex y no en crudo porque
                        # esto viaja en KISS, donde 0xC0 y 0xDB van escapados:
                        # ver CMD_RETO en protocolo.h.
                        libre.reto = binascii.hexlify(os.urandom(16))
                        c.sendall(bytes([FEND, CMD_RETO]) + libre.reto + bytes([FEND]))
                    c.sendall(bytes([FEND, CMD_ESTADO, FEND]))
                except OSError:
                    pass   # si el tubo ya esta roto, lo recoge el bucle de lectura

            elif tipo == "op":
                c, _ = clave.fileobj.accept()
                if r.nodo is None:
                    c.close()
                    continue
                # Por aqui van las contraseñas del WiFi en claro (`red`,
                # `red2`). Contra algo que no ha dicho quien es, NO se abre:
                # mas vale un "no se pudo" que regalarle la clave de una red
                # ajena a quien haya pillado la ranura.
                if r.nombre == "?":
                    r.dice("hay algo sin identificar en la ranura; no se abre")
                    c.close()
                    continue
                if EXIGE and not r.auth:
                    r.dice("no ha contestado al reto; no se abre")
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
                mira_estado(r, d, ranuras, sel)
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
            # Quien no se identifica no se queda con la ranura. Son CUATRO, y
            # cuatro conexiones calladas dejan a la celda del tejado sin forma
            # de administrarla —que es justo lo que este relevo existe para
            # evitar—. Los barridos de puertos que entran por aqui no saben
            # fabricar un EV_ESTADO, asi que se caen solos.
            # Un nodo con firmware viejo no contesta al reto: no da error, se
            # calla. Sin esta linea el modo "solo aviso" no avisa de nada y no
            # hay forma de ver como va el despliegue. Se dice UNA vez por
            # conexion, que si no el registro se llena.
            if (r.nodo and SECRETO and not EXIGE and not r.auth
                    and not r.avisado and ahora - r.desde > GRACIA_S):
                r.avisado = True
                r.dice("no contesta al reto (¿firmware sin secreto?);"
                       " aun no se exige, se le deja")
            if r.nodo and ahora - r.desde > GRACIA_S and (
                    r.nombre == "?" or (EXIGE and not r.auth)):
                r.dice("no dijo quien era en %d s; se cierra" % GRACIA_S
                       if r.nombre == "?" else
                       "no contesto al reto en %d s; se cierra" % GRACIA_S)
                sel.unregister(r.nodo); r.cierra_nodo()
                if r.op:
                    sel.unregister(r.op); r.cierra_op()
                continue
            if r.nodo and not r.op and ahora - r.ultimo_latido > LATIDO_S:
                r.ultimo_latido = ahora
                try:
                    r.nodo.sendall(bytes([FEND, CMD_ESTADO, FEND]))
                except OSError:
                    r.dice("se corto al latir")
                    sel.unregister(r.nodo); r.cierra_nodo()


if __name__ == "__main__":
    sys.exit(main())
