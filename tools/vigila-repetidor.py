#!/usr/bin/env python3
"""¿Sigue vivo el repetidor de allí arriba, y sigue repitiendo?

Un nodo instalado donde no llega ni WiFi ni Bluetooth no se puede consultar. Pero
**se le puede oír por la radio**, que es justo lo suyo, y hay dos preguntas
distintas que conviene no confundir:

  1. ¿ESTÁ VIVO?  Cada nodo emite su baliza de identificación cada 10 minutos —
     es obligación de estación, no adorno. Esa baliza trae su indicativo, con qué
     señal llega y hasta su batería. Si deja de aparecer, algo pasa.
  2. ¿REPITE?  Que emita no prueba que repita. Así que cada cierto tiempo se le
     manda una transmisión corta y se mira si **vuelve**: cuando el repetidor la
     repite, nuestro propio nodo la recibe otra vez y la descarta por duplicada,
     de modo que su contador `dup` sube. Ese salto es la prueba, y no hay otra
     forma de tenerla sin subir a mirarlo.

Se cuelga del nodo de casa (por WiFi) y va dejando el resultado en el registro:

    ./vigila-repetidor.py --tcp 192.168.1.50 --busco C31AG-9 [--prueba-cada 30]

Con `--aviso <fichero>` escribe además una línea de estado que puede leer
cualquier otra cosa (una web, un bot, un cron).
"""
import os, sys, time

AQUI = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, AQUI)
import importlib.util
spec = importlib.util.spec_from_file_location("nodo", os.path.join(AQUI, "nodo.py"))
nodo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nodo)

EV_HOLA, EV_ESTADO = 0x84, 0x85


def opt(a, nombre, tipo, defecto):
    if nombre in a:
        i = a.index(nombre)
        v = tipo(a[i + 1]); del a[i:i + 2]; return v
    return defecto


def ahora():
    return time.strftime("%d/%m %H:%M:%S")


def main():
    a = sys.argv[1:]
    tcp = opt(a, "--tcp", str, "")
    busco = opt(a, "--busco", str, "C31AG-9").upper()
    cada = opt(a, "--prueba-cada", float, 30.0)      # minutos entre pruebas de eco
    mudo = opt(a, "--mudo", float, 25.0)             # minutos sin baliza = alarma
    fichero = opt(a, "--aviso", str, "")
    if not tcp:
        print("hace falta --tcp <ip del nodo de casa>"); return 1

    print("-- vigilando a %s desde %s: baliza cada 10 min, eco cada %.0f min"
          % (busco, tcp, cada), flush=True)

    n = None
    ultima_baliza = 0.0
    ultimo_rssi = None
    ultima_bateria = None
    ultima_prueba = 0.0
    repite = None            # None = aun no probado
    alarmada = False

    def apunta(txt):
        print("[%s] %s" % (ahora(), txt), flush=True)
        if fichero:
            try:
                with open(fichero, "w") as f:
                    f.write("%s | %s | baliza:%s | rssi:%s | bat:%s | repite:%s\n" % (
                        ahora(), txt,
                        "-" if not ultima_baliza else
                        "hace %.0f min" % ((time.time() - ultima_baliza) / 60),
                        ultimo_rssi, ultima_bateria,
                        {None: "?", True: "si", False: "NO"}[repite]))
            except OSError:
                pass

    def estado_dup():
        """El contador de duplicados del nodo de casa: sube cuando algo vuelve.

        DOS DETALLES QUE COSTARON UN RATO:
          - Hay que **vaciar** lo que estuviera encolado antes de preguntar. El
            nodo manda su estado por su cuenta ante cualquier novedad, asi que
            la primera respuesta que se lee puede ser de hace medio minuto:
            medido, el contador real subia de 239 a 287 y aqui se leia 255 las
            dos veces. Un contador que no se mueve es lo mismo que un fallo.
          - Y hay que quedarse con el **ULTIMO** estado que llegue, no con el
            primero, por lo mismo."""
        for _ in n.lee(0.4):
            pass                       # a la basura lo viejo
        n.manda(nodo.CMD_ESTADO)
        ultimo = None
        for t, p in n.lee(2.0):
            if t == EV_ESTADO:
                for campo in p.decode("ascii", "replace").split(" "):
                    if campo.startswith("dup="):
                        ultimo = int(campo[4:])
        return ultimo

    while True:
        try:
            if n is None:
                n = nodo.Nodo(tcp=tcp)
                apunta("enlazado con el nodo de casa")

            # --- escucha pasiva: la baliza del de arriba ---
            for t, p in n.lee(5.0):
                if t == EV_HOLA and len(p) >= 9:
                    # [rssi][snr][src x3][stream][flags][bateria][indicativo]
                    quien = p[8:].decode("ascii", "replace").strip()
                    if busco in quien.upper():
                        ultima_baliza = time.time()
                        ultimo_rssi = int.from_bytes(p[0:1], "big", signed=True)
                        ultima_bateria = p[7]
                        repetidor = bool(p[6] & 0x01)
                        if not repetidor:
                            apunta("OJO: %s dice que NO esta repitiendo" % busco)
                        if alarmada:
                            alarmada = False
                            apunta("VUELVE: baliza de %s otra vez (%d dBm)"
                                   % (busco, ultimo_rssi))
                        else:
                            apunta("baliza de %s: %d dBm, bateria %d%%"
                                   % (busco, ultimo_rssi, ultima_bateria))

            # --- prueba activa: ¿repite? ---
            if time.time() - ultima_prueba > cada * 60:
                ultima_prueba = time.time()
                antes = estado_dup()
                # UN SEGUNDO DE TONO, no un INICIO suelto.
                #  Se probo con INICIO+FIN a secas y no volvia nada: hace falta
                #  una transmision de verdad para que el repetidor tenga algo
                #  que repetir. Un segundo de tono son dos lotes y ~0,3 s de
                #  aire cada media hora - nada.
                import math, struct
                raw = b"".join(struct.pack("<h", int(6000 * math.sin(
                    2 * math.pi * 700 * i / 8000.0))) for i in range(8000))
                cod, n_tramas, lotes = nodo.codifica(raw, "1200")
                n.manda(nodo.CMD_INICIO, bytes([cod]))
                for i, lote in enumerate(lotes):
                    n.manda(nodo.CMD_VOZ, bytes([cod, n_tramas]) + lote)
                    time.sleep(nodo.LOTE_MS / 1000.0)
                n.manda(nodo.CMD_FIN)
                time.sleep(2.5)
                despues = estado_dup()
                if antes is not None and despues is not None:
                    volvio = despues > antes
                    if repite != volvio:
                        apunta("eco: %s (dup %d -> %d)"
                               % ("REPITE" if volvio else "NO REPITE", antes, despues))
                    repite = volvio

            # --- alarma por silencio ---
            if ultima_baliza and not alarmada and \
               time.time() - ultima_baliza > mudo * 60:
                alarmada = True
                apunta("SIN NOTICIAS de %s desde hace %.0f min" % (busco, mudo))

        except KeyboardInterrupt:
            return 0
        except Exception as e:
            apunta("se corto (%s: %s); reintento en 10 s" % (type(e).__name__, e))
            try: n.s.close()
            except Exception: pass
            n = None
            time.sleep(10)


if __name__ == "__main__":
    sys.exit(main())
