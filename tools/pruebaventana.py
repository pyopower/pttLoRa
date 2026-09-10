#!/usr/bin/env python3
"""pruebaventana.py — la ventana de recepcion de la app, en Python, para poder
probarla sin un movil delante.

Es una TRADUCCION LITERAL de `encaja`/`bombea`/`seqAbsoluto` de NodoService.kt.
Si se toca uno hay que tocar el otro — el mismo pacto que hay entre el
`hash_indicativo` del firmware y el de nododatos.py, y por el mismo motivo: dos
implementaciones del mismo algoritmo que se separan en silencio.

Lo que se comprueba aqui es el ALGORITMO: el orden, el retraso, el relleno
entre los dos caminos, el hueco definitivo y la vuelta del seq de 8 bits. Lo
que NO se comprueba es el Kotlin ni el audio.
"""
RETRASO = 2          # el de fabrica es 1; aqui se prueba el caso mas exigente


class Ventana:
    def __init__(self):
        self.r = {}          # seq absoluto -> (datos, por_rf, rssi)
        self.sig = 1
        self.mayor = 0
        self.salida = []     # lo reproducido, en orden
        self.por_rf = 0
        self.tapados = 0
        self.huecos = 0

    def seq_absoluto(self, seq):
        d = ((seq - (self.sig & 0xFF)) + 256) % 256
        return self.sig - (256 - d) if d > 200 else self.sig + d

    def encaja(self, seq, datos, por_rf, rssi=-70):
        s = self.seq_absoluto(seq)
        if s < self.sig:
            return
        if s > self.mayor:
            self.mayor = s
        vieja = self.r.get(s)
        if vieja is None:
            self.r[s] = (datos, por_rf, rssi)
        elif por_rf and not vieja[1]:
            self.r[s] = (datos, True, rssi)

    def bombea(self, forzar=False):
        while True:
            if self.sig > self.mayor:
                return
            if not forzar and self.mayor < self.sig + RETRASO:
                return
            lote = self.r.pop(self.sig, None)
            self.sig += 1
            if lote is None:
                self.huecos += 1
                self.salida.append(None)
            else:
                self.por_rf += 1 if lote[1] else 0
                self.tapados += 0 if lote[1] else 1
                self.salida.append(lote[0])


def caso(nombre, f):
    v = Ventana()
    esperado, extra = f(v)
    ok = v.salida == esperado and all(getattr(v, k) == n for k, n in extra.items())
    print("%-4s %s" % ("OK" if ok else "MAL", nombre))
    if not ok:
        print("     salida  =", v.salida, "esperado =", esperado)
        print("     rf=%d tapados=%d huecos=%d  esperado %s"
              % (v.por_rf, v.tapados, v.huecos, extra))
    return ok


def todo_por_radio(v):
    for s in range(1, 7):
        v.encaja(s, "r%d" % s, True); v.bombea()
    v.bombea(True)
    return ["r1", "r2", "r3", "r4", "r5", "r6"], dict(por_rf=6, tapados=0, huecos=0)


def el_caso_del_usuario(v):
    """Por radio llegan 4 y el 5 no; ese faltante lo reemplaza el de Internet."""
    for s in (1, 2, 3, 4, 6, 7):
        v.encaja(s, "r%d" % s, True)
    v.encaja(5, "i5", False)              # solo Internet trajo el 5
    v.bombea(True)
    return ["r1", "r2", "r3", "r4", "i5", "r6", "r7"], dict(por_rf=6, tapados=1, huecos=0)


def internet_llega_antes(v):
    """Internet SIEMPRE se adelanta; la radio asciende a su ranura si llega a
    tiempo, y por eso la cuenta de radio es la de verdad."""
    for s in range(1, 6):
        v.encaja(s, "i%d" % s, False)     # primero toda la copia de Internet
        v.encaja(s, "r%d" % s, True)      # y detras la de radio
        v.bombea()
    v.bombea(True)
    return ["r1", "r2", "r3", "r4", "r5"], dict(por_rf=5, tapados=0, huecos=0)


def hueco_de_verdad(v):
    """Ni radio ni Internet: el lote se da por perdido cuando llegan dos
    posteriores, no antes."""
    for s in (1, 2, 4, 5, 6):
        v.encaja(s, "r%d" % s, True); v.bombea()
    v.bombea(True)
    return ["r1", "r2", None, "r4", "r5", "r6"], dict(por_rf=5, tapados=0, huecos=1)


def desordenados(v):
    """Llegan cambiados de orden dentro de la ventana y salen en su sitio."""
    for s in (2, 1, 4, 3, 5):
        v.encaja(s, "r%d" % s, True); v.bombea()
    v.bombea(True)
    return ["r1", "r2", "r3", "r4", "r5"], dict(por_rf=5, tapados=0, huecos=0)


def el_retraso_es_de_dos(v):
    """Con solo dos lotes dentro no puede salir nada: el primero aun podria
    llegar por el otro camino."""
    v.encaja(1, "r1", True); v.bombea()
    v.encaja(2, "r2", True); v.bombea()
    salida_a_medias = list(v.salida)
    v.encaja(3, "r3", True); v.bombea()
    assert salida_a_medias == [], salida_a_medias
    v.bombea(True)
    return ["r1", "r2", "r3"], dict(por_rf=3)


def la_vuelta_del_seq(v):
    """El seq es de 8 bits y da la vuelta cada 256 lotes (~2 min hablando)."""
    v.sig = 250; v.mayor = 249
    for s in [250, 251, 252, 253, 254, 255, 0, 1, 2]:
        v.encaja(s, "r%d" % s, True); v.bombea()
    v.bombea(True)
    return ["r250", "r251", "r252", "r253", "r254", "r255", "r0", "r1", "r2"], \
           dict(por_rf=9, huecos=0)


def un_lote_muy_atrasado(v):
    """Uno que llega despues de que su turno haya pasado no se cuela al final:
    se tira. Sonaria fuera de sitio."""
    for s in (1, 2, 3, 4, 5):
        v.encaja(s, "r%d" % s, True); v.bombea()
    v.encaja(1, "tarde", True)            # el 1 ya sono hace rato
    v.bombea(True)
    return ["r1", "r2", "r3", "r4", "r5"], dict(por_rf=5, huecos=0)


def el_fin_no_cierra(v):
    """⚠️ EL CASO QUE SE COMIA EL FINAL DE CADA FRASE.

    El FIN viaja por los dos caminos y por Internet llega ANTES que los ultimos
    lotes de radio. Si el FIN vacia la ventana de golpe, esos lotes llegan con
    su turno ya pasado y se tiran. Aqui el FIN solo anota que no habra mas: el
    cierre lo manda el reloj, despues de darles su margen."""
    for s in (1, 2, 3):
        v.encaja(s, "r%d" % s, True); v.bombea()
    # llega el FIN (por Internet, adelantado) -> NO se vacia
    antes = list(v.salida)
    # y ahora entran los rezagados de radio, que antes se perdian
    v.encaja(4, "r4", True); v.bombea()
    v.encaja(5, "r5", True); v.bombea()
    v.bombea(True)                       # el reloj, pasado el margen
    assert len(antes) < 5, antes
    return ["r1", "r2", "r3", "r4", "r5"], dict(por_rf=5, huecos=0)


def sin_colchon(v):
    """Colchon en «directo» (retraso 0): cada lote suena al llegar, sin
    reordenar y sin esperar a nadie. Es la salida de emergencia del usuario."""
    global RETRASO
    viejo, RETRASO = RETRASO, 0
    try:
        for s in (1, 2, 3):
            v.encaja(s, "r%d" % s, True); v.bombea()
        a_medias = list(v.salida)
        v.bombea(True)
        assert a_medias == ["r1", "r2", "r3"], a_medias   # nada retenido
    finally:
        RETRASO = viejo
    return ["r1", "r2", "r3"], dict(por_rf=3, huecos=0)


def solo_internet(v):
    """Un movil sin radio ninguna: el comodin lo trae todo y se ve que fue el."""
    for s in range(1, 5):
        v.encaja(s, "i%d" % s, False); v.bombea()
    v.bombea(True)
    return ["i1", "i2", "i3", "i4"], dict(por_rf=0, tapados=4, huecos=0)


def la_radio_se_cae_a_mitad(v):
    """Sombra a mitad de frase: la radio trae el principio, Internet el final."""
    for s in (1, 2, 3):
        v.encaja(s, "r%d" % s, True)
    for s in (4, 5, 6):
        v.encaja(s, "i%d" % s, False)
    v.bombea(True)
    return ["r1", "r2", "r3", "i4", "i5", "i6"], dict(por_rf=3, tapados=3, huecos=0)


if __name__ == "__main__":
    import sys
    casos = [
        ("todo por radio", todo_por_radio),
        ("4 por radio y el 5 lo pone Internet", el_caso_del_usuario),
        ("Internet se adelanta y la radio asciende", internet_llega_antes),
        ("hueco que no trajo nadie", hueco_de_verdad),
        ("llegan desordenados", desordenados),
        ("el retraso es de dos lotes", el_retraso_es_de_dos),
        ("la vuelta del seq de 8 bits", la_vuelta_del_seq),
        ("un lote que llega tarde se tira", un_lote_muy_atrasado),
        ("movil sin radio: solo comodin", solo_internet),
        ("la radio se cae a mitad de frase", la_radio_se_cae_a_mitad),
        ("el FIN no cierra: los rezagados llegan", el_fin_no_cierra),
        ("colchon en directo: nada se retiene", sin_colchon),
    ]
    malos = sum(0 if caso(n, f) else 1 for n, f in casos)
    print("\n%d de %d" % (len(casos) - malos, len(casos)))
    sys.exit(1 if malos else 0)
