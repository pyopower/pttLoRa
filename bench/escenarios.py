#!/usr/bin/env python3
"""escenarios.py — los casos con los que se decidio el diseño de la malla.

  ./venv/bin/python bench/escenarios.py

Correr esto ANTES de tocar el firmware cada vez que se cambie una regla de
repeticion. Es barato y contesta con numeros lo que discutir a ojo no contesta.
"""
import random
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simula import Sim
from malla import Red, cadena, racimo


def racimos_en_cadena(grupos, por_grupo, separacion, dispersion, semilla=7):
    """LA TOPOLOGIA REALISTA, y la que decide el diseño.

    No es una fila de nodos ni una nube: son NUBES ENCADENADAS. Grupos de gente
    en varios sitios, cada grupo denso (todos se ven entre si) y los grupos
    enlazados salto a salto. Es donde la inundacion a ciegas se muere y donde
    se ve para que sirve el TDMA."""
    r = random.Random(semilla)
    pos = []
    for g in range(grupos):
        cx = g * separacion
        for _ in range(por_grupo):
            pos.append((cx + r.uniform(-dispersion, dispersion),
                        r.uniform(-dispersion, dispersion)))
    return pos


def linea(etiqueta, r):
    print("  %-13s entrega %5.1f%%   CANAL %5.1f%%   lat p95 %6.0f ms"
          % (etiqueta, 100 * r['entrega'], 100 * r['canal_max'],
             r['latencia_p95']))


def main():
    casos = [
        ("RACIMO DE 8 (todos se ven)", Red(racimo(8, 0.3, semilla=3), 1.0)),
        ("RACIMO DE 15", Red(racimo(15, 0.3, semilla=5), 1.0)),
        ("CADENA DE 10 (9 saltos)", Red(cadena(10, 1.0), 1.05)),
        ("4 GRUPOS DE 5 EN CADENA", Red(racimos_en_cadena(4, 5, 1.0, 0.12), 1.05)),
    ]
    for nombre, red in casos:
        print("=== %s ===" % nombre)
        for e in ('inundacion', 'supresion'):
            linea(e, Sim(red, estrategia=e, lotes=12, saltos_max=12).corre())
        linea('tdma S=4', Sim(red, estrategia='tdma', lotes=12,
                              saltos_max=12, ranuras=4).corre())
        print()

    print("=== CUANTAS RANURAS: medido sobre 4 grupos de 5 en cadena ===")
    print("    (2 falla por geometria: el salto h+1 comparte ranura con el h-1,")
    print("     y el h+1 es el vecino inmediato del que esta escuchando)")
    red = Red(racimos_en_cadena(4, 5, 1.0, 0.12), 1.05)
    for S in (2, 3, 4, 5):
        r = Sim(red, estrategia='tdma', lotes=12, saltos_max=12, ranuras=S).corre()
        print("  S=%d  ranura %5.1f ms / paquete %.1f ms (guarda %4.1f ms)  "
              "entrega %5.1f%%   lat p95 %6.0f ms"
              % (S, 480.0 / S, r['taire'], 480.0 / S - r['taire'],
                 100 * r['entrega'], r['latencia_p95']))


def enlaces_internet():
    """EL PAPEL DEL ENLACE POR INTERNET, y por que hay que tocar el firmware.

    Un enlace es un atajo de **aire cero**: no gasta canal, no gasta saltos y
    llega antes que la radio. Los nodos que solo tienen RF no se enteran de
    nada — para ellos la pasarela es un vecino mas que emite. Esa es justo la
    propiedad que se quiere: que convivan.

    El caso que importa NO es "el que habla tiene Internet" (ese es facil), es
    **"la pasarela es un nodo cualquiera que le oyo por radio"**. Y ahi el
    firmware de hoy pierde mas de la mitad de lo que podria entregar."""
    print("=== EL ENLACE POR INTERNET: ¿se reinicia el presupuesto de saltos? ===")
    print("    Isla A: nodos 0-3 en fila; habla el 0; la pasarela es el 3 (3 saltos).")
    print("    Isla B: nodos 4-9, sin contacto por radio con A.  saltos_max=3.\n")
    pos = [(i, 0.0) for i in range(4)] + [(50.0 + i, 0.0) for i in range(6)]
    red = Red(pos, 1.05)

    def m(**kw):
        r = Sim(red, estrategia='tdma', ranuras=4, lotes=12, origen=0,
                saltos_max=3, **kw).corre()
        return ("entrega %5.1f%% (%d de 9)   CANAL %5.1f%%   lat p95 %6.0f ms"
                % (100 * r['entrega'], round(r['entrega'] * 9),
                   100 * r['canal_max'], r['latencia_p95']))

    print("  sin enlace                  ", m())
    print("  enlace 3<->4 CON reinicio   ", m(enlaces=[(3, 4)]))
    print("  enlace 3<->4 SIN reinicio   ", m(enlaces=[(3, 4)],
                                              reinicia_saltos=False))
    print("\n  'SIN reinicio' es lo que hace el firmware HOY: `del_enlace` llama a")
    print("  `procesar` con la trama tal cual, o sea con el presupuesto de saltos")
    print("  que le quedaba (aqui 0). Al otro lado solo la oye el vecino inmediato")
    print("  de la pasarela: el enlace, en vez de extender el alcance, lo AMPUTA.")


def robustez(topologias=10):
    """LA PRUEBA QUE DE VERDAD DECIDE: la MISMA topologia, muchas disposiciones.

    Una sola disposicion engaña, y engaño: con una sola salia que la supresion
    igualaba practicamente al TDMA, y con diez se ve que no. La supresion
    depende de como caigan los nodos —su espera la fija el RSSI, asi que hay
    disposiciones en las que dos nodos deciden hablar casi a la vez— mientras
    que el TDMA no depende de nada de eso.

    Correr SIEMPRE con varias disposiciones antes de creerse un numero."""
    print("=== ROBUSTEZ: 4 grupos de 5 en cadena, %d disposiciones ===" % topologias)
    tot = {'inundacion': [], 'supresion': [], 'tdma': []}
    for topo in range(1, topologias + 1):
        red = Red(racimos_en_cadena(4, 5, 1.0, 0.12, semilla=topo), 1.05)
        for e in tot:
            kw = {'ranuras': 4} if e == 'tdma' else {}
            if e == 'supresion':
                kw = {'jitter': (50, 150), 'azar_ms': 40}
            v = [Sim(red, estrategia=e, lotes=12, saltos_max=12,
                     semilla=sem, **kw).corre()['entrega'] for sem in (1, 2, 3)]
            tot[e].append(100.0 * sum(v) / len(v))
    for e in ('inundacion', 'supresion', 'tdma'):
        v = tot[e]
        print("  %-11s media %5.1f%%   peor caso %5.1f%%"
              % (e, sum(v) / len(v), min(v)))
    print("\n  La supresion casi DOBLA la entrega de la inundacion y no cuesta")
    print("  casi nada, pero es erratica. El TDMA es el que la deja en 100%.")


def arquitectura_celda(topologias=7):
    """CELDA vs MALLA — la comparacion que cambio el diseño (8-sep-2026).

    Idea del usuario, sacada de como funciona TETRA: en vez de que todos los
    nodos sean iguales y todos repitan, **dos papeles**. La CELDA es
    infraestructura (sitio fijo, corriente, antena, PA) y es la unica que
    repite; los CLIENTES solo emiten lo suyo y escuchan. Las celdas se enlazan
    entre si por Internet, no por el aire — igual que TETRA.

    Resulta que gana en todo: entrega, canal ocupado, latencia... y sobre todo
    en RIESGO DE INGENIERIA, porque la sincronia deja de ser un problema
    distribuido (la celda es el reloj) y eso era lo dificil del TDMA."""
    def pueblos(n_p, por, sep, disp, sem):
        r = random.Random(sem)
        pos = []
        for i in range(n_p):
            pos.append((i * sep, 0.0))            # la celda, en el centro
            for _ in range(por):
                pos.append((i * sep + r.uniform(-disp, disp),
                            r.uniform(-disp, disp)))
        return pos

    POR = 4
    celdas = [i * (POR + 1) for i in range(4)]
    enlaces = [(0, 5), (5, 10), (10, 15)]

    for sep, eti in ((1.5, "CERCA: las celdas se oyen por radio"),
                     (3.0, "LEJOS: las celdas NO se oyen por radio")):
        tot = {}
        for topo in range(1, topologias + 1):
            pos = pueblos(4, POR, sep, 0.35, topo)

            class RedCelda(Red):
                """La celda oye y se hace oir mas lejos: sitio alto, antena, PA."""
                def oye(self, a, b):
                    if a == b:
                        return False
                    r = self.alcance * (1.8 if (a in celdas or b in celdas) else 1.0)
                    return self.dist(a, b) <= r

            rc = RedCelda(pos, 1.0)
            casos = [
                ("celda sin red", dict(estrategia='celda', celdas=celdas, fdd=True)),
                ("celda + red", dict(estrategia='celda', celdas=celdas, fdd=True,
                                     enlaces=enlaces)),
                ("tdma S=4", dict(estrategia='tdma', ranuras=4)),
                ("supresion", dict(estrategia='supresion', jitter=(50, 150),
                                   azar_ms=40)),
            ]
            for n, kw in casos:
                v = [Sim(rc, lotes=12, saltos_max=12, semilla=s, **kw).corre()
                     for s in (1, 2)]
                tot.setdefault(n, []).append(
                    (100 * sum(x['entrega'] for x in v) / 2,
                     sum(x['latencia_p95'] for x in v) / 2))
        print("=== 4 pueblos, %s ===" % eti)
        for n in ("celda sin red", "celda + red", "tdma S=4", "supresion"):
            v = tot[n]
            print("  %-14s entrega %5.1f%%   lat p95 %6.0f ms"
                  % (n, sum(x[0] for x in v) / len(v),
                     sum(x[1] for x in v) / len(v)))
        print()
    print("  Cuando el hueco no lo cruza la radio, NO LO CRUZA NADA de la parte")
    print("  radio: ni TDMA ni supresion. Solo el enlace. Y donde la radio si")
    print("  llega, la celda gana igualmente y con menos latencia.")


def cadena_de_relevos():
    """CUANTO SE ESTIRA UNA CADENA DE CELDAS POR RADIO, SIN INTERNET.

    La pregunta del usuario (8-sep-2026): no cuatro celdas juntas, sino **cada
    celda al borde de la cobertura de la anterior**, encadenadas. Que es lo que
    de verdad interesa: una red autonoma por radio que cubra distancias grandes
    a base de cacharros, con Internet como salvavidas y no como cimiento.

    Y la respuesta es buena: **el canal ocupado NO CRECE con la longitud de la
    cadena**. La celda A y la celda D no se oyen, asi que emiten a la vez sin
    estorbarse — reutilizacion espacial. Lo unico que cuesta la longitud es
    LATENCIA (~70 ms por salto).

    Lo unico que limita es el PRESUPUESTO DE SALTOS, que hoy esta en 3 y es un
    campo de 4 bits: **maximo 15 relevos**. Todo lo que se pierde en las filas
    de `saltos=3` se pierde ahi, no en el aire.
    """
    print("=== CADENA DE RELEVOS: cada celda al borde de la anterior ===")
    print("    Cliente al principio. 'canal' es lo ocupado EN UN PUNTO.\n")
    print("  celdas  saltos  entrega   canal   latencia p95")
    for nc in (3, 5, 8, 12):
        for saltos in (3, nc):
            pos = [(-0.4, 0.0)] + [(i * 1.0, 0.0) for i in range(nc)]
            red = Red(pos, 1.05)
            r = Sim(red, estrategia='celda', celdas=list(range(1, nc + 1)),
                    origen=0, lotes=12, saltos_max=saltos, semilla=1).corre()
            print("    %2d      %2d    %5.1f%%   %5.1f%%      %5.0f ms%s"
                  % (nc, saltos, 100 * r['entrega'], 100 * r['canal_max'],
                     r['latencia_p95'],
                     '' if saltos == 3 else '   <- presupuesto ampliado'))
        print()
    print("  El canal se queda clavado en ~45% con 3 celdas y con 12: la cadena")
    print("  NO cuesta mas aire. Solo latencia. Y el limite son los 15 saltos")
    print("  que caben en el campo de 4 bits de la cabecera.")


if __name__ == '__main__':
    main()
    enlaces_internet()
    print()
    robustez()
    print()
    arquitectura_celda()
    print()
    cadena_de_relevos()
