# El transceptor autónomo (rama `transceptor`, en rodaje)

La placa LoRa SOLA, sin móvil ni app: micro-altavoz Bluetooth con PTT
(manos libres HFP), Codec2 dentro del ESP32 y la voz por el SX1278. La OLED es
el único instrumento.

Es el **producto estrella** del proyecto (ver el README, «Camino A»). La rama
está en rodaje: funciona en el aire en los dos sentidos con un micro Abbree,
pero aún quedan cosas abiertas (ver
«Siguiente» más abajo).

## Qué hay

| Dónde | Qué es | Estado |
|---|---|---|
| `firmware/` (`pio run -e transceptor`) | **El transceptor dentro del firmware principal**: malla, balizas, NVS y órdenes por USB, más el micro Bluetooth (`src/audio_bt.*`) como fuente y sumidero de `audio_local`, y el PTT separado de su origen (`src/ptt.*`) | 🟢 **Probado con un micro Abbree/KST_vHMIC010**: voz en los dos sentidos, PTT por AVRCP, pantalla OK. Emparejar por **doble RST validado** (4-oct); la sección web aún sin probar en un navegador |
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
- La pantalla es un **S-metro de aguja como el de un CB** (`pinta_transceptor`): arriba RX/TX en inverso, quién habla y la frecuencia; abajo un instrumento de doble escala (S1-S9/+20/+40 con S9 = −93 dBm, y la potencia en mW) con la aguja con inercia. Solo queda además un icono de Bluetooth: fijo con el micro enganchado, parpadeando si el emparejado no está, y nada si no hay ninguno. En la esquina contraria, el papel en la red con el mismo criterio con el que el nodo decide si repite (`hay_a_quien_repetir`): una torre si hay celda (y la placa se calla), una R en negativo si repite ella como digipeater, y nada si está sola. En reposo, en el sitio de RX/TX, la pila rellena según la carga (o un rayo por USB sin pila) y el último indicativo si cabe entero. Se refresca cada 100 ms mientras se mueve.

**Emparejar el micro sin consola** (4-oct-2026):

- **Doble RST**: pulsar RST dos veces seguidas (la segunda antes de 3 s). La
  pantalla pone «EMPAREJAR MICRO BT», busca hasta 3 × 10 s y se engancha al
  aparato de audio en modo emparejar con más señal. Lo detecta una marca en la
  NVS (`hfp/rst2`) que se borra a los 3 s de arrancar; sólo cuentan arranques
  por encendido/RST, no los reinicios por fallo. ⚠️ Abrir el puerto serie dos
  veces seguidas con un programa que mueva RTS también reinicia la placa y
  puede contar como doble RST.
- **Web** (`docs/index.html`, sección «Configurar la placa»): WebSerial por el
  USB → indicativo (CMD_CONFIG reenviando canal y saltos del estado) y lista de
  micros encontrados para elegir uno. La búsqueda escribe en el registro
  `micros: MAC|dBm|nombre` (de más a menos señal) y `micros: fin N`.

Órdenes (`tools/nodo.py`):

| Orden | Qué hace |
|---|---|
| `audio` | Estado: PTT, códec, colchón, heap y bloque libre, y el micro Bluetooth |
| `audio bt buscar` | 10 s buscando micros (el micro, en modo emparejar); lista `micros: MAC\|dBm\|nombre` |
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
- **El Abbree medido (2-oct-2026, sale como `KST_vHMIC010`)**, con btmon desde una
  Raspberry y luego en Android. Ofrece HFP, A2DP y AVRCP. Lo que manda depende de
  con qué perfiles se le conecte:
  - **Solo HFP**, como lo ve esta placa: el PTT manda un **`AT+BLDN` al pulsar y
    otro al soltar** (0,26 s entre los dos en una pulsación corta; con 36 s
    mantenido, el segundo llega al soltar). No hay AT propia de PTT.
  - **Con A2DP + AVRCP** (un móvil): el PTT pasa a teclas AVRCP, **FAST FORWARD al
    pulsar** (siempre la misma ráfaga: un toque y luego medio segundo mantenido) y
    **REWIND al soltar**, también con el audio de llamada abierto. Ahí `AT+BLDN` lo
    manda el botón **P1** (en Android, rellama al último número).
  - **+ / −** son volumen (`+VGS`).
  El firmware lo aprende solo: el primer `AT+BLDN` marca el micro como «PTT
  propio» (queda en la NVS con él), cada `AT+BLDN` conmuta el PTT, y desde ese
  momento el volumen deja de hacer de PTT. La cuenta es la del botón, no la de
  la emisión: si el nodo corta por TOT con el PTT pulsado, el `AT+BLDN` de soltar
  no vuelve a abrir. `nodo.py audio` lo enseña en `ptt=volumen|propio|propio(abajo)`.
  **Sin probar todavía en la placa**: falta ver si P1 también manda `AT+BLDN` con
  solo HFP (entonces abriría el PTT).
- **Pantalla segun la alimentacion** (3-oct-2026): con corriente externa, siempre
  encendida y un rayo pequeño tras la pila; con bateria, solo mientras transmite
  o recibe (o al pulsar PRG) y 5 s de cola. La LoRa32 v2.1 no tiene pin de USB,
  asi que hay que deducirlo, y **todavía no está resuelto**:
  - Por el **porcentaje** (versión 1) no vale: por USB con la pila al 60 %, la
    carga apenas le gana al consumo del Bluetooth y se quedó en 59-60 % una hora
    entera; la pantalla temporizaba enchufada.
  - Por la **tensión** (versión 2, la actual: escalón de ±40 mV frente a la media
    del minuto anterior, y tendencia en 20 min): enchufada acierta, pero al
    **desenchufar no lo vio** (rayo puesto y pantalla fija a pilas). Con la carga
    casi igual al consumo, el escalón al quitar el USB es menor de 40 mV.
  - **Siguiente prueba**: el chip USB-serie se alimenta del USB, así que su TX
    (GPIO3, el RX del ESP32) debería estar en alto con USB y caído sin él. Leerlo
    con `gpio_get_level(GPIO_NUM_3)`, guardar en RAM cada 10 s ese nivel y los mV,
    y volcarlo al registro cuando vuelva el USB para ver qué señal separa de
    verdad los dos casos. Con un cargador sin datos el chip también se alimenta,
    así que valdría igual.
- **Pantalla que no volvía a encenderse** (3-oct-2026): `despierta_pantalla()`
  ponía `pantalla_on = true` sin mandar nada al panel, y `gestiona_pantalla()`
  creía que ya estaba encendida. Tras el primer apagado se quedaba a oscuras para
  siempre. Ahora solo apunta la actividad; encender (reiniciando el SSD1306 con
  `oled.begin()`, porque un `DISPLAYON` tras `DISPLAYOFF` no la encendía en
  nodoCASA) lo hace solo `gestiona_pantalla()`. Validado: despierta con TX, RX y
  balizas y se apaga a los 5 s.
- **Congelación de las 22:42** (3-oct-2026): el volcado de cuelgue (partición
  coredump en 0x3f0000, `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`) decía **«stack
  overflow in task loopTask»**. La pila de `loop()` pasa de 8 a 16 KB
  (`SET_LOOP_TASK_STACK_SIZE`) y el registro apunta cada nuevo mínimo
  (`pila loop: minimo N B libres`); en reposo con el micro, ~13,4 KB libres.
  Guardar el ELF de cada versión flasheada (`pttlora/backup/elf/`): el volcado
  solo se puede leer con el ELF exacto (`esp_coredump info_corefile -c volcado.bin
  firmware.elf`), y el de esa versión no se guardó.
- **Zumbido y enlace roto por claves viejas** (3-oct-2026): tras restaurar una
  **copia completa de la flash** (con la NVS de otro día) volvieron las claves de
  emparejamiento antiguas: el micro mandaba paquetes casi todos a cero, A2DP se
  cerraba con «motivo 1», zumbido en el Abbree y desconexión a los pocos
  segundos. No era la antena ni nada externo. **Arreglo: olvidar el micro
  (`audio` 7) y volver a emparejar**; el Abbree se pone solo en emparejamiento al
  borrarlo de la placa. **Nunca restaurar una copia completa de la flash**; para
  volver atrás, solo la partición de la aplicación.
  Desde entonces el diagnóstico (`diag:` cada 5 s, en `audio_bt.cpp`) da 16000 B/s
  en cada sentido y 0-7 paquetes vacíos de 667 por cada 5 s.
- **Abbree en la placa (3-oct-2026)**: engancha (HFP, CVSD 8 kHz) y por su
  altavoz se oye lo que llega por LoRa. Pero con el audio abierto **sus botones
  no mandan nada por HFP** (ni PTT, ni P1, ni +/-): el PTT va por AVRCP, que el
  firmware no tenia. **Resuelto con AVRCP** (3-oct-2026): la placa se registra
  como fuente A2DP (sin mandar musica nunca) para que el Abbree abra AVRCP, y
  recibe FAST FORWARD = PTT abajo, REWIND = PTT arriba. Validado de punta a
  punta: Abbree -> placa -> LoRa -> celda -> reflector -> app, y al reves.
  Ojo al probar con la app: si tiene el MISMO indicativo que la placa, el
  servicio de datos no se la reenvia (anti-eco); usar un sufijo (C31AG-7).
- **Codificar Codec2 se ha encarecido**: con Bluedroid haciendo SCO + A2DP +
  AVRCP en el mismo núcleo 0, codificar una trama tarda **33-39 ms de media y
  41-52 ms en el peor caso**, para tramas de 40 ms (el banco medía 22,5 ms). Va
  justo o tarde: hay que moverlo (ver «Siguiente»).
- **Vigilante de tareas**: `loop()` no suelta nunca la CPU 1 y ESP-IDF, por
  defecto, vigila la tarea de reposo de esa CPU. Reinició nodoCASA en reposo a los
  276 s («reset=wdt-tarea»). Se apaga como en Arduino a secas
  (`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n` en `sdkconfig.defaults`).

## Siguiente (por orden)

1. ~~Llevar el transceptor al firmware principal~~, ~~separar el PTT de su
   origen~~, ~~probarlo con un micro~~ y ~~el PTT propio del Abbree~~: **hecho y
   validado en el aire** (3-oct-2026, PTT por AVRCP). La OLED a oscuras era
   `despierta_pantalla()` (ver arriba).
   **Ahora, por orden:**
   a. **Detectar el USB de verdad** (ver «Pantalla según la alimentación»): medir
      GPIO3 con y sin USB.
   b. **Codec2 fuera del núcleo de Bluedroid**: el codificador ya no cabe en 40 ms.
      Probar la tarea de audio en el núcleo 1 ahora que Bluedroid carga más el 0,
      o bajar la prioridad/mover lo que no sea SCO.
   c. **Quitar P1 (`AT+BLDN`) como PTT**: con A2DP+AVRCP el PTT es AVRCP y P1 no
      debe transmitir (decisión del usuario: «inadmisible P1 como PTT»).
   d. Decidir qué diagnósticos se quedan (`diag:` cada 5 s, `oled: despertar`,
      `pila loop`) y quitar el resto. Entender las AT `+IPHONEACCEV=1,1,N`: es la
      batería del micro (N de 9), se podría enseñar.
   e. Con batería, la pantalla despierta también con cada baliza de la celda (una
      por minuto): que solo despierte con voz.
2. Medir el heap mínimo hablando y escuchando con el micro enganchado. Si aprieta,
   recortar la cola de recepción o los modos de Codec2 que se escuchan.
3. ~~El PTT del Abbree~~: hecho (AVRCP). Con el micro pegado a nodoCASA engancha
   aunque no tenga antena de Bluetooth.
4. Avisos hablados (prompts de Piper en Codec2 guardados en flash) y teclas
   auxiliares.
5. Flasher web con un cuestionario mínimo (indicativo, potencia, canal) por WebSerial.
   El flasher de `docs/` sale de `main`.
6. **Roger beep (propuesto, sin hacer)**: NO codificar el tono en la voz (Codec2 a
   1200 destroza los tonos y gasta aire), sino una MARCA en la trama de INICIO o FIN
   («viene de un transceptor autónomo») con la que cada receptor, sea la app o una
   placa, genera el pitido en local. Así suena limpio, cada uno puede quitarlo y la app
   puede poner un icono de «autónomo». Comprobar antes que los receptores viejos ignoran
   el byte de más. Además, un bip local en el propio altavoz al soltar el PTT, que no
   sale al aire.

## La pantalla, criterios

Sencilla, como un pequeño transceptor de CB. **No cargarla de datos**: los
contadores, el heap y el estado en texto salen con `nodo.py audio` / `estado`.
- La OLED es monocroma (SSD1306): para destacar algo hay tamaño, parpadeo y vídeo inverso.
- La pila va sin porcentaje, solo con el relleno; el sitio es para el indicativo.
- Un indicativo sale entero o no sale: a medias podría ser el de otra estación.
- `audio captura [1|2|3]` vuelca la pantalla para verla desde otra máquina (con 1, 2 y 3
  es una demostración, sin emitir).
