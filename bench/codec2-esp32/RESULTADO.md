# ¿Cabe Codec2 dentro del ESP32? — medido el 12-sep-2026

Medido en una **LilyGO LoRa32 v2.1 de verdad** (la de `nodoMOVIL`, restaurada
después a su v1.52), ESP32 a 240 MHz, `-Os`, con **voz real** de
`src/voz.h` (Piper) y no con un tono — el cuantificador de Codec2 busca en un libro
de códigos y un seno es el caso fácil.

El porcentaje es **sobre el tiempo real**: 100 % significa que procesar una
trama cuesta lo mismo que dura esa trama de audio, o sea que no queda ni un
ciclo para nada más.

| modo | trama | bytes | codificar | descodificar |
|------|-------|-------|-----------|--------------|
| 3200 | 20 ms | 8 | 5.604 µs · **28,0 %** | 7.005 µs · **35,0 %** |
| 2400 | 20 ms | 6 | 5.736 µs · 28,7 % | 7.108 µs · 35,5 % |
| 1600 | 40 ms | 8 | 10.985 µs · 27,5 % | 13.856 µs · 34,6 % |
| 1300 | 40 ms | 7 | 10.207 µs · 25,5 % | 14.269 µs · 35,7 % |
| **1200** | 40 ms | 6 | 18.020 µs · **45,1 %** | 15.121 µs · **37,8 %** |
| 700C | 40 ms | 4 | 38.213 µs · **95,5 %** | 12.139 µs · 30,3 % |

**Memoria**: codificador y descodificador vivos a la vez ocupan **61.624 B de
heap** (66.840 B en 700C), y la tarea necesita **~18,7 KB de pila** — Codec2
trabaja con vectores en la pila, no en el heap. Total **~80 KB de RAM**.

**Flash** (restando un sketch de Arduino vacío, 266.933 B): **~73 KB** con sólo
el modo 1200, **~170 KB** con los seis modos. La ranura de OTA tiene 1,92 MB y
el firmware v1.52 ocupa 1,17 MB: **quedan 790 KB**, así que cabe de sobra.

## Lo que significa

**Sí cabe, y con margen** — pero el margen sale de un detalle que cambia el
resultado por completo: **esto es simplex**. Nunca se codifica y se descodifica
a la vez; o hablas o escuchas. Así que el número que manda no es la suma, es el
peor de los dos por separado: **45 % de un núcleo** en el modo 1200, y el ESP32
tiene dos.

**El 700C queda descartado**: 95,5 % de un núcleo sólo para codificar. Es el
modo más barato en el aire y el más caro en CPU, y ahí no hay margen para el
jitter, ni para LoRa, ni para nada.

**El 1200 —el modo por defecto del proyecto— pasa**, y con holgura si se le
dedica el núcleo 1. Si algún día apretara, el 1300 cuesta la mitad de CPU al
codificar (25,5 % contra 45,1 %) por un byte más de trama.

## Lo que NO está medido

- **Con la radio y el BLE trabajando al lado.** Esto se midió con la placa
  haciendo sólo esto. LoRa va por SPI y gasta poco, pero el BLE roba ciclos y
  tiene sus propios plazos. Falta repetirlo con el firmware completo.
- **El heap real en runtime.** El firmware v1.52 se lleva 76 KB de RAM estática
  y deja ~251 KB, pero NimBLE reserva lo suyo al arrancar. Los 80 KB del códec
  entran, pero hay que verlo con todo en marcha.

## Cómo repetirlo

    ./preparar.sh                       # clona codec2 y genera los codebooks
    pio run -e banco -t upload          # los seis modos
    pio run -e banco-1200 -t upload     # sólo el 1200, para medir flash
    pio device monitor

⚠️ Y la trampa que costó el primer intento: sin `-D__EMBEDDED__` los libros de
códigos de Codec2 se declaran `static float` y aterrizan en DRAM — el enlazador
se queja de 25 KB de más y no cabe ni el banco vacío. Con esa bandera son
`static const float` y viven en flash. Codec2 lo trae pensado; hay que pedirlo.

---

# ¿Y un firmware DEDICADO al micrófono Bluetooth? — medido el 12-sep-2026

Idea del usuario: si Classic no convive con NimBLE, que no convivan — un
firmware aparte, sin enlace con el móvil, que libere lo que necesita. Medido
todo con el mismo método (sketch mínimo menos sketch vacío, `-Os`, misma placa):

| pieza | flash | RAM estática |
|---|---|---|
| Arduino pelado | 266.933 B | 21.464 B |
| **+ Bluetooth Classic con HFP** | **+799.884 B** | +17.460 B |
| **+ NimBLE** (lo que hay hoy) | +321.820 B | +14.204 B |
| **+ Codec2**, un solo modo | +73.500 B | ~80 KB en marcha |

El «+828 kB del controlador Classic» que estaba anotado en `main.cpp` era buena
memoria: son 781 KB.

## La cuenta

    firmware v1.52 completo         1.167.849 B
    − NimBLE (fuera el móvil)        −321.820 B
    + Classic con HFP                +799.884 B
    + Codec2 (modo 1200)              +73.500 B
    ─────────────────────────────────────────────
                                    1.719.413 B
    ranura de OTA                   1.966.080 B
    MARGEN                            246.667 B  (12,5 %)

**Cabe** — y eso sin renunciar al WiFi ni a la pantalla, que también se podrían
quitar de un firmware así.

## Lo que aprieta no es el flash, es la RAM

Estática quedaría en ~79 KB (76 − NimBLE + Classic). Encima, en marcha:
**~80 KB de Codec2** (heap + pila) y **~90 KB que se lleva Bluedroid**. Total
~249 KB de los 327,7 KB de DRAM: quedan unos **78 KB para todo lo demás** —
buffers de radio, la malla, la cola de audio. Entra, pero ahí ya no hay sitio
para descuidos.

## Y el trabajo de verdad no es el tamaño

⚠️ **Hay que recompilar el framework.** El core de Arduino precompilado trae
`CONFIG_BTDM_CTRL_BR_EDR_MAX_SYNC_CONN=0` — **cero conexiones SCO**, que es el
canal por donde viaja el audio de HFP — y además el datapath en PCM. Con ese
core no hay audio Bluetooth de ninguna manera. Hace falta ESP-IDF con Arduino
como componente y dos opciones cambiadas:

    CONFIG_BTDM_CTRL_BR_EDR_MAX_SYNC_CONN=1
    CONFIG_BTDM_CTRL_BR_EDR_SCO_DATA_PATH_HCI=y

Y después, escribir el manos libres: el nodo tiene que hacerse pasar por un
teléfono (perfil AG), atender los AT del auricular, abrir y cerrar el audio con
el PTT, y tratar CVSD a 8 kHz — que, al menos, es exactamente lo que come
Codec2, sin remuestrear nada.

**Lo que se pierde** en ese firmware: la app. Sin BLE no hay ajustes desde el
móvil, ni OTA desde el móvil, ni la rueda de quién ha hablado. La configuración
tendría que ir por serie o por WiFi.
