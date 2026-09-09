#!/usr/bin/env python3
"""malla.py — banco de simulacion de la malla de PTT LoRa.

POR QUE EXISTE
--------------
Las decisiones que quedan por tomar (¿supresion por RSSI con que parametros?
¿TDMA con 3 ranuras o con 4? ¿como deduce un nodo su distancia en saltos cuando
el terreno no hace anillos limpios?) **no se pueden validar con dos placas y un
repetidor en el rellano**. Aqui se contestan con numeros, y el firmware se
escribe una sola vez.

Es la misma disciplina que ya salio bien dos veces el 8-sep-2026: banco primero.

QUE MODELA, Y QUE NO
--------------------
Modela lo que decide el resultado:
  * el TIEMPO EN EL AIRE real de cada paquete (formula de Semtech, la misma que
    da 71,8 ms para un lote de 480 ms a SF7/BW250/CR4:5),
  * quien oye a quien (alcance de comunicacion) y quien ESTORBA a quien
    (alcance de interferencia, que siempre es mayor — es la razon de que dos
    ranuras no basten),
  * el EFECTO CAPTURA de LoRa: con ~6 dB de diferencia el receptor engancha el
    mas fuerte y descarta el resto,
  * que dos emisiones de LOS MISMOS BITS bien alineadas **no se destruyen**
    (transmision concurrente), que es lo que hace que el TDMA escale en
    densidad.

NO modela: desvanecimientos, terreno real, deriva de reloj mas alla de una
guarda fija, ni la diversidad constructiva (se queda en captura, que es la
hipotesis conservadora). Si algo sale bien aqui, en el aire saldra igual o
peor — nunca mejor.
"""

import math
import random


# ------------------------------------------------------------------ radio ---
def aire_ms(carga_B, sf, bw_Hz=250000, cr=1, preambulo=8, crc=1, cabecera=True):
    """Tiempo en el aire de un paquete LoRa, en ms. Formula de Semtech.

    Es la misma cuenta que da 71,8 ms para 80 B a SF7/BW250, que es el numero
    con el que se ha razonado todo el diseño. Si esto cambia, cambia todo.
    """
    ts = (2 ** sf) / float(bw_Hz)
    de = 1 if ts > 0.016 else 0            # low data rate optimize
    ih = 0 if cabecera else 1
    num = 8 * carga_B - 4 * sf + 28 + 16 * crc - 20 * ih
    n = 8 + max(math.ceil(num / (4.0 * (sf - 2 * de))) * (cr + 4), 0)
    return ((preambulo + 4.25) * ts + n * ts) * 1000.0


CABECERA_B = 8          # cabecera del protocolo (ver protocolo.h)
CAPTURA_dB = 6.0        # ventaja necesaria para que el receptor engancha uno


def carga_voz(bits_trama, ms_trama, lote_ms):
    """Bytes de un lote de voz: cabecera + las tramas de codec que caben."""
    return CABECERA_B + math.ceil(int(lote_ms / ms_trama) * bits_trama / 8.0)


MODOS = {'3200': (64, 20), '1200': (48, 40), '700C': (28, 40)}


# ------------------------------------------------------------------- red ----
class Red:
    """Nodos en un plano, con alcance de comunicacion y de interferencia.

    ⚠️ EL ALCANCE DE INTERFERENCIA ES MAYOR QUE EL DE COMUNICACION, y esa
    diferencia no es un detalle: es **la razon de que dos ranuras no basten**.
    Un nodo al que ya no le llega la señal lo bastante limpia como para
    decodificarla, sigue oyendola lo bastante como para estropearle la
    recepcion a otro. El factor 1,6 es el que se usa habitualmente.
    """

    def __init__(self, posiciones, alcance, factor_interferencia=1.6):
        self.pos = list(posiciones)
        self.n = len(self.pos)
        self.alcance = float(alcance)
        self.r_int = self.alcance * factor_interferencia

    def dist(self, a, b):
        (xa, ya), (xb, yb) = self.pos[a], self.pos[b]
        return math.hypot(xa - xb, ya - yb)

    def oye(self, a, b):
        return a != b and self.dist(a, b) <= self.alcance

    def estorba(self, a, b):
        return a != b and self.dist(a, b) <= self.r_int

    def potencia_dB(self, a, b):
        """Perdida por distancia, referida al alcance. Solo se usa para decidir
        el efecto captura, asi que basta con que sea monotona."""
        d = max(self.dist(a, b), 1.0)
        return -20.0 * math.log10(d / 1.0)

    def vecinos(self, a):
        return [b for b in range(self.n) if self.oye(a, b)]

    def saltos_reales(self, origen):
        """Distancia en saltos de cada nodo al origen (anchura primero).
        Es la VERDAD del terreno; lo que cada nodo cree saber es otra cosa."""
        d = {origen: 0}
        frente = [origen]
        while frente:
            sig = []
            for a in frente:
                for b in self.vecinos(a):
                    if b not in d:
                        d[b] = d[a] + 1
                        sig.append(b)
            frente = sig
        return d


# --------------------------------------------------------------- cadena -----
def cadena(n, paso=1.0):
    """Nodos en fila. Es el peor caso para la reutilizacion de ranuras y el
    mejor para ver cuantos saltos aguanta la cosa."""
    return [(i * paso, 0.0) for i in range(n)]


def rejilla(lado, paso=1.0):
    return [(i * paso, j * paso) for j in range(lado) for i in range(lado)]


def racimo(n, radio, semilla=1):
    """Nodos al azar en un circulo: densidad alta, muchos se ven entre si.
    Es el caso que hoy revienta (todos repiten lo mismo)."""
    r = random.Random(semilla)
    return [(r.uniform(-radio, radio), r.uniform(-radio, radio))
            for _ in range(n)]


if __name__ == '__main__':
    print("Tiempo en el aire (BW 250 kHz, CR 4:5, preambulo 8):")
    for modo, (b, ms) in MODOS.items():
        for lote in (480, 960):
            pl = carga_voz(b, ms, lote)
            for sf in (7, 8):
                print("  %-5s lote %3d ms  SF%d  %3d B  %6.1f ms"
                      % (modo, lote, sf, pl, aire_ms(pl, sf)))
    print()
    red = Red(cadena(8, paso=1.0), alcance=1.05)
    print("Cadena de 8 nodos, alcance 1,05 pasos:")
    print("  vecinos del nodo 3:", red.vecinos(3))
    print("  saltos desde el 0:", red.saltos_reales(0))
