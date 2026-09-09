#!/usr/bin/env python3
"""simula.py — motor de simulacion de la malla. Ver malla.py para el modelo.

Compara tres formas de repetir la voz:

  INUNDACION   lo de hoy: todo el que oye algo nuevo lo repite tras una espera
               ALEATORIA de 50-200 ms. Coste O(nodos): cada repetidor a la
               vista mete otra copia en el aire.

  SUPRESION    el que lo oyo MAS FLOJO repite ANTES (es el mas lejano, el que
               mas alcance añade); el que ya oyo la repeticion de otro se calla.
               Coste ~O(1) por salto sin sincronizar nada. Es lo que hace
               Meshtastic.

  TDMA         supertrama en S ranuras; cada nodo emite en la ranura
               (su distancia en saltos) mod S. Los que estan a la misma
               distancia emiten LOS MISMOS BITS A LA VEZ -> captura, no
               interferencia. Coste O(1) y escala en densidad Y en saltos.
"""

import heapq
import random

from malla import Red, aire_ms, carga_voz, MODOS, CAPTURA_dB, cadena, racimo, rejilla


class Sim:
    def __init__(self, red, sf=7, modo='1200', lote_ms=480, lotes=20,
                 origen=0, estrategia='inundacion', saltos_max=8,
                 ranuras=4, semilla=1, jitter=(50, 200),
                 enlaces=(), enlace_ms=60.0, reinicia_saltos=True,
                 azar_ms=0.0, celdas=(), fdd=False, intrusos=()):
        self.red = red
        self.lote_ms = float(lote_ms)
        self.lotes = lotes
        self.origen = origen
        self.estrategia = estrategia
        self.saltos_max = saltos_max
        self.ranuras = ranuras
        self.jitter = jitter
        self.r = random.Random(semilla)
        bits, ms = MODOS[modo]
        self.carga = carga_voz(bits, ms, lote_ms)
        self.taire = aire_ms(self.carga, sf)
        self.ranura_ms = self.lote_ms / ranuras

        self.cola = []            # eventos (t, orden, tipo, datos)
        self.orden = 0
        self.log = []             # todas las emisiones: (ini, fin, nodo, pkt, salto)
        self.activas = []
        self.visto = [set() for _ in range(red.n)]     # dedupe por nodo
        self.emitido = [set() for _ in range(red.n)]   # ya repetido por nodo
        self.pendiente = {}       # (nodo,pkt) -> t previsto, para poder cancelar
        self.salto_creido = [{} for _ in range(red.n)]  # pkt -> saltos que cree
        self.recibido_en = {}     # (nodo,pkt) -> t de la primera recepcion buena
        self.aire_nodo = [0.0] * red.n

        # ---- ENLACES POR INTERNET ----
        # Un atajo de aire CERO entre dos puntos de la malla. No gasta canal, no
        # gasta saltos y llega antes que la radio. Cada extremo se convierte en
        # un ORIGEN LOCAL de su isla de RF, y los nodos que solo tienen radio no
        # se enteran de nada: para ellos es un vecino mas que emite.
        self.enlaces = {}
        for a, b in enlaces:
            self.enlaces.setdefault(a, []).append(b)
            self.enlaces.setdefault(b, []).append(a)
        self.enlace_ms = float(enlace_ms)
        # ⚠️ ¿SE REINICIA EL PRESUPUESTO DE SALTOS AL CRUZAR EL ENLACE?
        # Tiene que reiniciarse: al otro lado hay una isla de RF nueva y entera.
        # Si no, una trama que ya gasto 2 de sus 3 saltos llega a la otra punta
        # con UN salto de presupuesto — y entonces el enlace por Internet, en
        # vez de extender el alcance, lo AMPUTA. Se deja conmutable justo para
        # poder medir la diferencia.
        self.reinicia_saltos = reinicia_saltos
        # ⚠️ UNA PIZCA DE AZAR ENCIMA DE LA ESPERA POR RSSI, Y NO ES ADORNO.
        # Medido: con la espera puramente determinista el resultado depende de
        # la FASE accidental entre la ola de repeticiones y el lote siguiente —
        # la misma red daba 100% con una ventana y 55% con otra, sin ruido de
        # por medio (la espera determinista da lo mismo con cualquier semilla).
        # Eso es un diseño fragil: funciona o no segun un numero que nadie
        # eligio. El azar rompe esa alineacion sistematica.
        # El orden lo sigue poniendo el RSSI; el azar solo desempata.
        self.azar_ms = float(azar_ms)

        # ---- ARQUITECTURA DE CELDA (estilo TETRA) ----
        # Dos papeles en vez de uno. La CELDA es infraestructura: sitio fijo,
        # corriente, antena buena, y es la unica que repite. Los CLIENTES solo
        # emiten lo suyo y escuchan; no repiten nada.
        # Con eso desaparece la inundacion donde hay cobertura: cliente ->
        # celda -> todos, UNA repeticion, con cualquier numero de clientes.
        self.celdas = set(celdas)
        # ¿Subida y bajada en frecuencias distintas? Es lo que hace TETRA (y
        # cualquier repetidor de radioaficionado). Cuesta un segundo modulo en
        # la celda y a cambio **los clientes dejan de oirse entre si**: se acaba
        # el nodo oculto y la emision de un cliente ya no puede destrozar la
        # bajada que estan escuchando los demas.
        self.fdd = bool(fdd)
        # INTRUSOS: nodos que emiten FUERA DE TURNO. No es un caso rebuscado —
        # en una red de aficionados siempre habra alguien con firmware viejo, el
        # reloj desviado, o sencillamente un segundo que aprieta el PTT. Es la
        # prueba de fuego de si separar subida y bajada en frecuencias distintas
        # sirve de algo: con una sola frecuencia, ese intruso destroza la BAJADA
        # que estan escuchando todos los demas.
        # Cada uno: (nodo, periodo_ms, desfase_ms).
        self.intrusos = list(intrusos)

    # ---------------------------------------------------------- eventos ----
    def _mete(self, t, tipo, datos):
        self.orden += 1
        heapq.heappush(self.cola, (t, self.orden, tipo, datos))

    def _canal_ocupado(self, nodo, t):
        """CAD: ¿oigo a alguien emitiendo ahora mismo?"""
        for (ini, fin, quien, _p, _s) in self.activas:
            if ini <= t < fin and self.red.oye(nodo, quien):
                return True
        return False

    def _emite(self, nodo, pkt, salto, t):
        fin = t + self.taire
        tx = (t, fin, nodo, pkt, salto)
        self.log.append(tx)
        self.activas.append(tx)
        self.aire_nodo[nodo] += self.taire
        self.emitido[nodo].add(pkt)
        self._mete(fin, 'fin', tx)

    # ------------------------------------------------------- recepciones ---
    def _quien_lo_recibe(self, tx):
        """Quien decodifica esta emision, teniendo en cuenta captura.

        Dos emisiones del MISMO paquete no se destruyen (mismos bits, es lo que
        hace viable el TDMA). Dos emisiones DISTINTAS que se solapan solo dejan
        pasar una, y solo si gana por CAPTURA_dB."""
        ini, fin, quien, pkt, _s = tx
        buenos = []
        for otro in range(self.red.n):
            if not self.red.oye(otro, quien):
                continue
            if otro in (quien,):
                continue
            # ¿estaba emitiendo? entonces no oye nada (half duplex)
            sordo = any(a[2] == otro and a[0] < fin and a[1] > ini
                        for a in self.log)
            if sordo:
                continue
            # ¿hay otra emision DISTINTA solapada que le estorbe?
            estorbo = False
            for (i2, f2, q2, p2, _s2) in self.log:
                if q2 == quien or p2 == pkt:
                    continue                      # mismos bits: no estorba
                if f2 <= ini or i2 >= fin:
                    continue                      # no se solapa
                if not self.red.estorba(otro, q2):
                    continue
                if self.fdd and ((quien in self.celdas) != (q2 in self.celdas)):
                    continue          # una va por la subida y la otra por la bajada
                gana = (self.red.potencia_dB(otro, quien)
                        - self.red.potencia_dB(otro, q2))
                if gana < CAPTURA_dB:
                    estorbo = True
                    break
            if not estorbo:
                buenos.append(otro)
        return buenos

    # ------------------------------------------------------------ correr ---
    def corre(self):
        for k in range(self.lotes):
            self._mete(k * self.lote_ms, 'origen', k)
        for (nodo, periodo, desfase) in self.intrusos:
            t = desfase
            marca = 10000
            while t < self.lotes * self.lote_ms:
                self._mete(t, 'intruso', (nodo, marca))
                marca += 1
                t += periodo

        while self.cola:
            t, _o, tipo, d = heapq.heappop(self.cola)

            if tipo == 'origen':
                pkt = d
                self.visto[self.origen].add(pkt)
                self.salto_creido[self.origen][pkt] = 0
                self._propaga_enlace(self.origen, pkt, t)   # tambien por Internet
                self._emite(self.origen, pkt, 0, t)

            elif tipo == 'intruso':
                nodo, pkt = d
                self.visto[nodo].add(pkt)
                self.emitido[nodo].add(pkt)
                self._emite(nodo, pkt, 0, t)

            elif tipo == 'fin':
                self.activas = [a for a in self.activas if a is not d]
                for nodo in self._quien_lo_recibe(d):
                    self._llega(nodo, d, t)

            elif tipo == 'enlace':
                nodo, pkt, salto = d
                if pkt in self.emitido[nodo]:
                    continue                  # ya lo repitio: no hay nada que hacer
                if pkt in self.visto[nodo]:
                    # ⚠️ YA LE HABIA LLEGADO POR RADIO, Y AUN ASI IMPORTA.
                    # Si la copia de radio llego antes, este nodo cree estar a
                    # (por ejemplo) 3 saltos y va a repetir con el presupuesto
                    # agotado. La copia por Internet trae presupuesto ENTERO: es
                    # una distancia MEJOR, y la regla del diseño es que gana el
                    # minimo salto visto, venga por donde venga. Sin esto, la
                    # copia de radio "envenena" a la pasarela y el enlace no
                    # sirve para nada — que es justo lo que medimos.
                    if salto >= self.salto_creido[nodo].get(pkt, 99):
                        continue
                self.visto[nodo].add(pkt)
                self.salto_creido[nodo][pkt] = salto
                self.recibido_en.setdefault((nodo, pkt), t)
                self._propaga_enlace(nodo, pkt, t)
                if salto > self.saltos_max:
                    continue
                self._programa(nodo, pkt, salto, t, fuerza_origen=True)

            elif tipo == 'repite':
                nodo, pkt, salto = d
                if self.pendiente.get((nodo, pkt)) != t:
                    continue                       # cancelado
                self.pendiente.pop((nodo, pkt), None)
                if self.estrategia != 'tdma' and self._canal_ocupado(nodo, t):
                    # CAD: el canal esta ocupado, se reintenta un poco despues.
                    # En TDMA no se escucha antes de emitir: en tu ranura emites
                    # a ciegas, y precisamente por eso no chocas.
                    nuevo = t + 20
                    self.pendiente[(nodo, pkt)] = nuevo
                    self._mete(nuevo, 'repite', d)
                    continue
                self._emite(nodo, pkt, salto, t)

        return self.resultados()

    def _propaga_enlace(self, nodo, pkt, t):
        """Manda la trama por Internet a los nodos enlazados con este."""
        for otro in self.enlaces.get(nodo, ()):
            if pkt in self.visto[otro]:
                continue
            salto = 0 if self.reinicia_saltos else self.salto_creido[nodo][pkt]
            self._mete(t + self.enlace_ms, 'enlace', (otro, pkt, salto))

    def _llega(self, nodo, tx, t):
        _i, _f, _q, pkt, salto_emisor = tx
        if (nodo, pkt) not in self.recibido_en:
            self.recibido_en[(nodo, pkt)] = t

        mio = salto_emisor + 1
        # La distancia que cree cada nodo: el MINIMO visto para ese paquete.
        # El terreno no hace anillos limpios y un nodo puede oir dos distancias.
        ant = self.salto_creido[nodo].get(pkt)
        self.salto_creido[nodo][pkt] = mio if ant is None else min(ant, mio)

        if pkt in self.emitido[nodo]:
            return
        if pkt in self.visto[nodo]:
            # ya lo tenia: en SUPRESION esto es la señal de callarse
            if self.estrategia == 'supresion':
                self.pendiente.pop((nodo, pkt), None)
            return
        self.visto[nodo].add(pkt)
        self._propaga_enlace(nodo, pkt, t)
        if mio > self.saltos_max or nodo == self.origen:
            return
        self._programa(nodo, pkt, mio, t, emisor=_q)

    def _programa(self, nodo, pkt, mio, t, emisor=None, fuerza_origen=False):
        if fuerza_origen:
            # Llego por Internet: este nodo hace de origen de SU isla de radio.
            # En TDMA eso significa la ranura 0, igual que el origen de verdad.
            # Y si hay varias pasarelas en la misma isla, emiten los MISMOS bits
            # en la MISMA ranura: se refuerzan, no se estorban. Sale gratis.
            espera = 0.0 if self.estrategia != 'tdma' else self._hasta_ranura(0, t)
            self.pendiente[(nodo, pkt)] = t + espera
            self._mete(t + espera, 'repite', (nodo, pkt, mio))
            return

        if self.estrategia == 'celda':
            # Solo repite la celda, y en cuanto puede: no hay a quien esperar,
            # porque no hay otros repetidores con los que chocar.
            if nodo not in self.celdas:
                return
            espera = 5.0

        elif self.estrategia == 'inundacion':
            espera = self.r.uniform(*self.jitter)

        elif self.estrategia == 'supresion':
            # ESPERA INVERSA A LO FUERTE QUE SE OYO. El que lo oyo mas flojo es
            # el mas lejano: es el que mas alcance añade, asi que repite el
            # primero. Los que lo oyeron fuerte esperan, le oyen y se callan.
            lejos = self.red.dist(nodo, emisor) / self.red.alcance   # 0..1
            espera = self.jitter[0] + (1.0 - min(lejos, 1.0)) * (
                self.jitter[1] - self.jitter[0])
            if self.azar_ms:
                espera += self.r.uniform(0.0, self.azar_ms)

        else:   # tdma: en la ranura que toca, sin esperas al azar
            espera = self._hasta_ranura(
                self.salto_creido[nodo][pkt] % self.ranuras, t)

        cuando = t + espera
        self.pendiente[(nodo, pkt)] = cuando
        self._mete(cuando, 'repite', (nodo, pkt, self.salto_creido[nodo][pkt]))

    def _hasta_ranura(self, ranura, t):
        base = (t // self.lote_ms) * self.lote_ms
        objetivo = base + ranura * self.ranura_ms
        while objetivo < t:
            objetivo += self.lote_ms
        return objetivo - t

    def _ocupacion(self):
        """Fraccion de tiempo que el CANAL esta ocupado, visto desde cada nodo.

        ⚠️ ESTA ES LA METRICA QUE IMPORTA, y no "cuantas emisiones hubo".
        El aire que gasta un nodo (una emision por lote) sale igual en las tres
        estrategias y no distingue nada. Lo que las distingue es cuanto tiempo
        esta el medio ocupado ALREDEDOR de un nodo, porque eso es lo que le
        impide recibir y lo que satura el canal.
        Y hay una diferencia crucial: dos emisiones del MISMO paquete a la vez
        (TDMA) ocupan el canal UNA vez, no dos. Por eso se calcula sobre la
        UNION de los intervalos y no sumandolos."""
        dur = self.lotes * self.lote_ms
        peor = 0.0
        for nodo in range(self.red.n):
            trozos = sorted((i, f) for (i, f, q, _p, _s) in self.log
                            if q != nodo and self.red.estorba(nodo, q))
            ocupado, fin_ant = 0.0, -1.0
            for (i, f) in trozos:
                if i > fin_ant:
                    ocupado += f - i
                    fin_ant = f
                elif f > fin_ant:
                    ocupado += f - fin_ant
                    fin_ant = f
            peor = max(peor, ocupado / dur)
        return peor

    # --------------------------------------------------------- resultados --
    def resultados(self):
        # ⚠️ LA ENTREGA SE MIDE SOBRE **TODA LA RED**, no sobre "los que la radio
        # podria alcanzar". Estuvo escrito asi y era inservible: descontaba de la
        # cuenta a los nodos inalcanzables, con lo cual dos islas sin contacto por
        # radio salian al 100% de entrega y el enlace por Internet no parecia
        # aportar NADA. La metrica escondia justo lo que se queria medir.
        alcanzables = set(range(self.red.n)) - {self.origen}
        total = len(alcanzables) * self.lotes
        # SOLO los lotes de voz de verdad. Los paquetes de un intruso llevan
        # marca >= 10000 y contarlos daba "entregas" por encima del 100%.
        ok = sum(1 for (n, p) in self.recibido_en
                 if n in alcanzables and p < self.lotes)
        dur = self.lotes * self.lote_ms
        lat = [self.recibido_en[(n, p)] - p * self.lote_ms
               for (n, p) in self.recibido_en
               if n in alcanzables and p < self.lotes]
        lat.sort()
        return {
            'entrega': ok / total if total else 0.0,
            'emisiones': len(self.log),
            'emisiones_por_lote': len(self.log) / float(self.lotes),
            'aire_max': max(self.aire_nodo) / dur,
            'canal_max': self._ocupacion(),
            'aire_medio': sum(self.aire_nodo) / (self.red.n * dur),
            'latencia_p50': lat[len(lat)//2] if lat else float('nan'),
            'latencia_p95': lat[int(len(lat)*0.95)] if lat else float('nan'),
            'taire': self.taire,
        }
