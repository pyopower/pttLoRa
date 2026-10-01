# El transceptor autónomo (rama `transceptor`, en obras)

La placa LoRa SOLA, sin móvil ni app: micro-altavoz Bluetooth con PTT
(manos libres HFP), Codec2 dentro del ESP32 y la voz por el SX1278. La OLED es
el único instrumento.

Esta rama es **preliminar**: lo que hay funciona en banco y en el aire, pero
todavía en piezas sueltas. Se publica para poder trabajar sobre ella.

## Qué hay

| Dónde | Qué es | Estado |
|---|---|---|
| `firmware/` (`pio run -e transceptor`) | **El transceptor dentro del firmware principal**: malla, balizas, NVS y órdenes por USB, más el micro Bluetooth (`src/audio_bt.*`) como fuente y sumidero de `audio_local`, y el PTT separado de su origen (`src/ptt.*`) | 🟡 Compila y arranca en la placa, y el banco mide bien. **Sin probar todavía con micro ni en el aire** |
| `bench/hfp-ag/` (`pio run -e transceptor`) | **Transceptor v0**: firmware aparte, HFP-AG + Codec2 1200 + LoRa + pantalla | ✅ Probado en el aire en los dos sentidos (micro JBL GO ↔ celda ↔ reflector ↔ app), sin WiFi |
| `bench/hfp-ag/` (`pio run -e hfpag`) | Lo mismo pero la voz va por WiFi al reflector | Funciona, pero el WiFi y el audio Bluetooth se pisan (se pierde ~10 % del micro): queda como demostración |
| `firmware/src/audio_local.*` (`pio run -e lora32-audio`) | Codec2 DENTRO del firmware principal, como un tubo más (`T_LOCAL`), con el mismo arbitraje de PTT, TOT y eco que la app | ✅ Baliza hablada codificada en la placa y oída en la app |
| `bench/codec2-esp32/` | Banco de Codec2 en el ESP32 y generador de los libros de códigos | Lo usa `firmware/preparar-audio.sh` |

## El entorno `transceptor` del firmware principal

```
micro BT ──SCO──► audio_bt (anillo + control de ganancia) ──► audio_local (Codec2) ──► T_LOCAL ──► orden() ──► aire / enlace
altavoz  ◄──SCO── audio_bt (anillo + ganancia)            ◄── audio_local (colchón + ritmo real) ◄── entrega_al_anfitrion
botón PRG / pulsador / botones del micro / consola ──► ptt ──► audio_local
```

- **Arduino como componente de ESP-IDF** (`framework = arduino, espidf`), con la
  configuración en `firmware/sdkconfig.defaults` y `firmware/CMakeLists.txt`. Los demás
  entornos siguen siendo Arduino a secas y no leen nada de eso.
- **Sin BLE** (`-DSIN_BLE`): Bluedroid Classic y NimBLE no pueden convivir. No hay app
  por Bluetooth; se configura por USB.
- **WiFi apagado aunque haya red guardada.** Se enciende a propósito con
  `nodo.py audio wifi 1`. Medido el 1-oct en nodoCASA: sin WiFi quedan **127 KB** de heap;
  con WiFi y enlace, unos 85 KB, y con 67 KB `codec2_create` revienta (un assert en
  `nlp_create`, no devuelve NULL). Por eso ahora cada códec se crea a través de
  `crea_codec()`, que mira antes el heap libre y el bloque contiguo más grande, y si no
  hay sitio lo dice y no habla.
- **El TOT es el del nodo** (`vigila_ptt`, 3 min). Cuando corta a la propia placa se
  avisa al audio (`audio_ptt_denegado`), que suelta el PTT: con un micro Bluetooth el PTT
  es un conmutador y si no nadie lo soltaría.
- **Lo recibido se guarda codificado** (6 B por trama) y se descodifica a ritmo real
  cuando hay 6 tramas (240 ms) de colchón. Al altavoz va siempre algo, voz o silencio,
  como en el v0, para que no haya chasquidos.
- **Los avisos de la tarea de audio y de la pila Bluetooth van por una cola** y los
  suelta `loop()`: escribir en los tubos desde otra tarea pisaba los buffers.
- **Registro de ESP-IDF solo en ERROR**: va por la misma UART que el KISS.
- La pantalla es un **S-metro de aguja como el de un CB** (`pinta_transceptor`): arriba RX/TX en inverso, quién habla y la frecuencia; abajo un instrumento de doble escala (S1-S9/+20/+40 con S9 = −93 dBm, y la potencia en mW) con la aguja con inercia. Solo queda además un icono de Bluetooth, que parpadea si el micro no está enganchado. Se refresca cada 100 ms mientras se mueve.

Órdenes (`tools/nodo.py`):

| Orden | Qué hace |
|---|---|
| `audio` | Estado: PTT, códec, colchón, heap y bloque libre, y el micro Bluetooth |
| `audio bt buscar` | 10 s buscando micros (el micro, en modo emparejar) |
| `audio bt conecta AA:BB:CC:DD:EE:FF` | Lo vincula. Se recuerda en NVS y se reconecta solo cada 15 s |
| `audio bt olvida` | Lo desvincula |
| `audio ptt` | Abre o cierra el micro como un botón más. **EMITE** |
| `audio boton 0\|1\|2` | Botón PRG: solo pantalla, PTT mientras se mantiene (por defecto) o conmutador |
| `audio wifi 0\|1` | WiFi del transceptor (apagado por defecto) |
| `audio banco 16` | Mide Codec2 con todo en marcha, **sin emitir** |
| `audio captura [1\|2\|3]` | Vuelca la pantalla por USB (128×64 en texto); 1, 2 y 3 = demostración de TX, RX y reposo con pila, con datos inventados, **sin emitir** |

PTT del micro: botones de volumen (vol− abre, vol+ cierra, como en el v0), «asistente de
voz» (BVRA) y cualquier orden AT no estándar que contenga `PTT` (`=P`/`=R`, `=1`/`=0`,
`ON`/`OFF`, `DOWN`/`UP`). Las órdenes AT que no se reconocen se apuntan en el registro:
así se verá qué manda el Abbree.

⚠️ **Abrir el puerto serie reinicia la placa** (el CH9102 mueve DTR/RTS). Para probar
seguido conviene un proceso que lo tenga abierto todo el rato.

## Compilar

```sh
cd firmware && ./preparar-audio.sh        # una vez: trae y prepara Codec2 en lib-audio/
pio run -e lora32-audio                   # firmware principal con audio local
cd ../bench/hfp-ag && pio run -e transceptor   # el transceptor v0
```

Trampas de `bench/hfp-ag` (Arduino como componente de ESP-IDF 4.4.7):
- El Python de ESP-IDF trae `schema` 0.7.8 y el gestor de componentes revienta:
  `~/.platformio/penv/.espidf-4.4.7/bin/pip install 'schema==0.7.5'`.
- Si se toca `sdkconfig.defaults`, borrar `sdkconfig.hfpag` / `sdkconfig.transceptor`
  (se generan la primera vez y después mandan ellos).
- Arduino LIBERA la memoria del Bluetooth al arrancar si `btInUse()` es falso:
  hay que redefinirla con `extern "C"`.
- Codificar y descodificar en cada trama (eco) se come el 97 % de un núcleo y salta
  el WDT de IDLE: siempre simplex y `vTaskDelay(1)` en cada vuelta.
- El callback de salida SCO tiene que entregar SIEMPRE lo que se le pide (silencio
  si no hay nada); si no, chasquidos en reposo.
- La fuente de Adafruit GFX es de 7 bits: **en la pantalla, texto sin tildes**.

## Lo que se ha medido

- Codec2 1200 en el ESP32: codificar 22,5 ms y descodificar 15,5 ms por trama de 40 ms.
- Con Bluedroid Classic + Codec2 y sin WiFi quedan unos 98 KB de heap libres. Con WiFi
  bajan a unos 44 KB.
- En el firmware principal la tarea de audio va en el **núcleo 0**, que es lo medido.
  En el 1 (el de `loop()`) el códec llega tarde casi siempre.
- PTT del JBL: el botón «tel» sin llamada casi nunca manda nada. Los de volumen
  llegan siempre (`+VGS`), así que de momento vol− = PTT abajo y vol+ = arriba. El
  micro Abbree (hecho para POC) debería traer un PTT propio, que probablemente
  llega como una orden AT no estándar (`ESP_HF_UNAT_RESPONSE_EVT`). El banco ya
  registra todas las que recibe.

## Siguiente (por orden)

1. ~~Llevar el transceptor al firmware principal~~ y ~~separar el PTT de su
   origen~~: **hecho** (ver arriba). Falta **probarlo con un micro**: emparejar el JBL,
   oír por el altavoz lo que llega de la red y, con permiso, hablar por el aire.
2. Medir el heap mínimo hablando y escuchando con el micro enganchado. Si aprieta,
   recortar la cola de recepción o los modos de Codec2 que se escuchan.
3. El PTT del Abbree, cuando llegue: ver qué manda (las AT desconocidas salen en el
   registro).
4. Avisos hablados (prompts de Piper en Codec2 guardados en flash) y teclas
   auxiliares.
5. Flasher web con un cuestionario mínimo (indicativo, potencia, canal) por WebSerial.

⚠️ Todo lo que **emite** lleva `NOCALL` por defecto. Pon tu indicativo antes de salir
al aire.
