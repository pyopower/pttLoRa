# El transceptor autónomo (rama `transceptor`, en obras)

La placa LoRa SOLA, sin móvil ni app: micro-altavoz Bluetooth con PTT
(manos libres HFP), Codec2 dentro del ESP32 y la voz por el SX1278. La OLED es
el único instrumento.

Esta rama es **preliminar**: lo que hay funciona en banco y en el aire, pero
todavía en piezas sueltas. Se publica para poder trabajar sobre ella.

## Qué hay

| Dónde | Qué es | Estado |
|---|---|---|
| `bench/hfp-ag/` (`pio run -e transceptor`) | **Transceptor v0**: firmware aparte, HFP-AG + Codec2 1200 + LoRa + pantalla | ✅ Probado en el aire en los dos sentidos (micro JBL GO ↔ celda ↔ reflector ↔ app), sin WiFi |
| `bench/hfp-ag/` (`pio run -e hfpag`) | Lo mismo pero la voz va por WiFi al reflector | Funciona, pero el WiFi y el audio Bluetooth se pisan (se pierde ~10 % del micro): queda como demostración |
| `firmware/src/audio_local.*` (`pio run -e lora32-audio`) | Codec2 DENTRO del firmware principal, como un tubo más (`T_LOCAL`), con el mismo arbitraje de PTT, TOT y eco que la app | ✅ Baliza hablada codificada en la placa y oída en la app. Fuente de prueba y sumidero nulo: **todavía sin micro ni altavoz de verdad** |
| `bench/codec2-esp32/` | Banco de Codec2 en el ESP32 y generador de los libros de códigos | Lo usa `firmware/preparar-audio.sh` |

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

1. **Llevar el transceptor al firmware principal** como entorno `transceptor`
   (`framework = arduino, espidf`), para que tenga malla, balizas, configuración
   en NVS y órdenes por USB:
   - un módulo de manos libres (HFP-AG) que sea la **fuente** (micro con su control
     automático de ganancia) y el **sumidero** (altavoz a ritmo real, con colchón
     contra el jitter) de `audio_local`;
   - sin NimBLE en ese entorno: Bluedroid Classic y NimBLE no pueden convivir. El
     transceptor no tiene app; se configura por USB;
   - WiFi apagado por defecto, por la convivencia;
   - la pantalla del v0 (TX/RX, S-metro, TOT, estado del micro, batería), alimentada
     con el estado del nodo.
2. **Separar el PTT de su origen**: un único sitio que reciba el botón de la placa
   (GPIO 0 en la LoRa32, 38 en la T-Beam), un pulsador externo a masa, los botones
   del micro Bluetooth y la consola. Habrá modo pulsador (mantener) y modo
   conmutador (una pulsación abre y otra cierra). El TOT ya lo pone el nodo.
3. El PTT del Abbree, cuando llegue: ver qué manda con el banco.
4. Avisos hablados (prompts de Piper en Codec2 guardados en flash) y teclas
   auxiliares.
5. Flasher web con un cuestionario mínimo (indicativo, potencia, canal) por WebSerial.

⚠️ Todo lo que **emite** lleva `NOCALL` por defecto. Pon tu indicativo antes de salir
al aire.
