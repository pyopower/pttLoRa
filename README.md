# PTT LoRa

**Hablar por voz con un grupo donde no hay cobertura de nada.** Una placa LoRa
de unos 30 € y tu móvil Android: sin operador, sin cuota, sin internet y sin
repetidor de nadie. La cobertura la pones tú.

Para **radioaficionados con licencia**: va en 70 cm, sin cifrar, y cada estación
se identifica con su indicativo.

**Está funcionando, no es una idea.** Voz real entre móviles a través de la
radio, y **3,2 km sin visión directa con 50 mW y la antena de la caja**, sin
perder un solo paquete.

---

## Qué hace falta

**Una placa LoRa de 433 MHz y un móvil Android.** Nada más — ni cuota, ni
cobertura, ni servidor, ni internet.

⚠️ **La placa tiene que ser la versión de 433 MHz.** LilyGO vende las mismas
placas en 433, 868 y 915: la de 868/915 lleva un SX1276 que **no sintoniza**
esta banda y no sirve. Míralo antes de comprar, que es el error más caro y el
más fácil de cometer.

| placa | chip | qué añade | entorno |
|---|---|---|---|
| **LilyGO LoRa32 v2.1** (T3 v1.6.1) | ESP32 + SX1278 | pantalla OLED, USB-C | `lora32` |
| **LilyGO T-Beam v1.2** | ESP32 + SX1278 | **GPS**, batería 18650, gestión AXP2101 | `tbeam` |

Las dos valen para todo. La T-Beam es la de llevar encima: batería y GPS propio.
La LoRa32 es la de dejar puesta en un sitio.

**El móvil**: cualquiera con **Android 4.4 o posterior**. Se conecta a la placa
por Bluetooth LE o por WiFi, y la app no necesita Play Services ni cuenta de
nada. Aviso por experiencia: **el Bluetooth LE va fino de Android 5 en
adelante**; en 4.4 da guerra, y ahí es mejor conectar por WiFi — la placa
levanta su propia red si se le pide.

**La antena importa más que la placa.** Las medidas de arriba son con una antena
micro SMA de las que vienen en la caja; con algo decente en alto, otra historia.

---

## Cómo funciona

```
                       ┌──────────────────────────────┐
                       │  reflector  (un servidor)    │   opcional: une
                       └───────┬──────────────┬───────┘   zonas que no
                    Internet   │              │   Internet se oyen entre sí
   ─── ZONA A ─────────────────┼──────        ┼───────────── ZONA B ───
                               ▼              ▼
                          ┌─────────┐    ┌─────────┐   en alto, con antena.
                          │  CELDA  │    │  CELDA  │   REPITE SIEMPRE y es
                          └────┬────┘    └────┬────┘   la que da cobertura
                               │              │
              radio LoRa,      │              │
              439,600 MHz  ┌───┴───┐          │
                           │       │          │
                       ┌───┴──┐ ┌──┴───┐  ┌───┴──┐   NO repiten: donde hay
                       │ nodo │ │ nodo │  │ nodo │   celda no hace falta,
                       └───┬──┘ └───┬──┘  └───┬──┘   y así no se estorban
                     BLE ó │  WiFi  │         │
                          📱       📱        📱      hablas desde aquí
```

**Tres papeles, un solo firmware, y la placa elige sola:**

- **Celda** — la pones en alto, con corriente y buena antena. Repite todo lo que
  oye, y es la que convierte tres placas sueltas en una red que cubre un valle.
  Si tiene internet, además une tu zona con otras por un reflector.
- **Nodo** — la que llevas encima. Habla con tu móvil por Bluetooth o WiFi y se
  calla cuando hay una celda a la vista, para no estorbar.
- **Suelto** — sin ninguna celda cerca, dos placas en el campo **ya hacen red**
  ellas solas y se repiten la una a la otra. No hay nada que configurar.

```
  Sin infraestructura, sin cobertura y sin internet:

      📱── nodo ──────RF──────► nodo ──📱      y aquí sí se repiten
```

---

## Para qué sirve

Hablar por voz donde no hay nada. Es un walkie de grupo que **no depende de
ninguna red**, y al que le pones cobertura tú poniendo una celda en un sitio
alto.

- **Montaña, valles, pistas forestales.** Un grupo con móviles y una placa cada
  uno se oye a kilómetros sin repetidor de nadie.
- **Un pueblo o un valle entero** con una sola celda en un tejado.
- **Emergencias y simulacros**: funciona con la infraestructura caída, y una
  placa con batería en un collado abre el paso a otro valle.
- **Salir en el mapa**: la posición de cada uno viaja por la misma radio, y
  desde una celda con internet se publica en APRS-IS y se ve en aprs.fi.
- **Experimentar**: el protocolo cabe en una página y las herramientas son
  Python suelto. Se puede escuchar la red entera con un script de treinta
  líneas.

Lo que **no** es: no llega a donde llega un repetidor de FM con 25 W, no da
calidad de teléfono —es Codec2 a 1200 bps, se entiende bien y suena a radio— y
no es privado: va sin cifrar y a propósito.

---

## Uso legal

**Esto es para radioaficionados con licencia.** El perfil de fábrica —439,600
MHz, 250 kHz de ancho, 17 dBm, voz sin cifrar— está pensado para el servicio de
aficionados y para ningún otro.

- **Comprueba tu plan nacional antes de encender.** 430-440 MHz está atribuida
  al servicio de aficionados en las tres regiones de la UIT, pero **el reparto
  interno lo fija cada país** y no coincide. 439,600 con 250 kHz de ancho ocupa
  `439,475-439,725`: mira qué hay ahí en tu plan y en el de tus vecinos, porque
  un canal así se oye lejos. Nadie ha hecho ese trabajo por ti.
- **Tu indicativo no es opcional.** El nodo lo baliza y lo mete en la cabecera de
  cada transmisión: eso es la identificación de tu estación. Un nodo sin
  configurar sale como `NOCALL` y no debe transmitir así.
- **No hay cifrado, y es a propósito.** Ocultar el contenido está prohibido en el
  servicio de aficionados (RR 25.2A). Quien busque privacidad, éste no es el
  proyecto.

El firmware **se niega a salir de 430-440 MHz** (`BANDA_MIN`/`BANDA_MAX` en
`platformio.ini`): rechaza cualquier orden de radio fuera de banda y, si
encuentra guardada una frecuencia que no vale, vuelve al canal de fábrica al
arrancar. No pretende detener a nadie —es código abierto y cambiar dos líneas
son dos minutos— sino evitar lo único que iba a pasar de verdad: un dedo torpe
escribiendo 443 en vez de 439 y transmitiendo fuera de banda sin enterarse.
Quien tenga otra atribución cambia esas dos líneas, recompila, y con ello asume
lo que emite.

## Empezar

**1 · Flashea la placa.**

```bash
cd firmware
pio run                                    # compila lora32 y tbeam
pio run -e lora32 -t upload                # con la placa conectada por USB
```

**2 · Compila e instala la app.** Codec2 no va en el repositorio; lo trae el
guión:

```bash
cd app
./preparar.sh                              # clona y parchea Codec2
./gradlew assembleRelease                  # el APK sale en app/build/outputs/
```

**3 · Enciende, empareja y habla.** Abre la app, entra en **Ajustes**, escribe
**tu indicativo** —sin él el nodo sale como `NOCALL` y no debe transmitir— y
elige el nodo: aparece por Bluetooth, o por WiFi si la placa está en tu red. Y
ya está: pulsa para hablar.

Un consejo para la primera prueba: **baja la potencia a 2 dBm** si tienes las dos
placas en la misma mesa. A 17 dBm y veinte centímetros se satura el receptor de
la de al lado y verás tramas corruptas sin motivo aparente.

```bash
tools/nodo.py radio 439.600 --potencia 2   # para el banco
```

### El papel de cada placa

**Un solo firmware hace los tres papeles**, y se eligen desde la app sin
reflashear: obligar a elegir binario es pedirle al usuario un conocimiento que
no tiene por qué tener.

| Perfil | Qué hace |
|---|---|
| **Automático** (por defecto) | Puente para tu móvil, y repetidor sólo cuando hace falta: se calla si ve una celda o si no hay a quién repetir |
| **Repetidor fijo** (celda) | Repite siempre. Es el de la placa que se pone en alto: puede haber estaciones que le oigan a él y no entre ellas |
| **Sólo mi radio** | No repite nunca. Para quien no quiera gastar batería ni aire en los demás |

## Herramientas

```bash
tools/nodo.py estado
tools/nodo.py config EA1ABC --canal 1 --saltos 3 --potencia 2
tools/nodo.py escuchar 30
tools/nodo.py hablar grabacion.wav --modo 1200
tools/prueba2nodos.py --segundos 4        # dos nodos, los dos puertos abiertos
tools/pruebawifi.py 192.168.4.1 --radio /dev/ttyACM1   # varios usuarios a la vez
tools/pruebaenlace.py --a 192.168.4.1 --b /dev/ttyACM1 # enlace entre dos nodos
tools/nodovirtual.py --escucha 4461       # punto de reunión en un servidor
tools/vigila.py /dev/ttyACM1              # lee por el cable el código de acceso
bench/bench.py voz.wav                    # comparar modos de Codec2 con pérdidas
tools/ota_bt.py fw.bin --tcp 192.168.1.50 # actualizar el firmware por el enlace
tools/nododatos.py --escucha 4460 --reflector 127.0.0.1:4461   # camino de datos
tools/igate.py --conf igate.conf          # posiciones -> APRS-IS
```

Las tres piezas de servidor —reflector, nodo de datos e igate— son procesos
Python sueltos sin dependencias: se ponen en cualquier máquina con un
`systemd` de diez líneas.

⚠️ **`ota_bt.py` no necesita la clave del WiFi.** Va por el propio enlace KISS
del puerto 4460, así que sirve igual por cable, por BLE, por WiFi o por un canal
de mando saliente. La OTA de Arduino del 3232 sí la pide, y ésa es la diferencia
que hace que se pueda actualizar un nodo instalado en una red ajena.

---

## Cómo está hecho por dentro

A partir de aquí es detalle técnico: sirve para modificarlo o para escribir
otro cliente, y no hace falta para usarlo.

## Perfil de canal

| Parámetro | Valor |
|---|---|
| Frecuencia | 439.600 MHz |
| Ancho de banda | 250 kHz |
| Spreading factor | 7 |
| Coding rate | 4/5 |
| Sync word | 0x3B |
| Códec | Codec2, modo en la trama (por defecto 1200 bps) |
| Lote | 480 ms de audio por paquete |

**El alcance no se compra con SF.** A SF9/125 kHz no cabe ni Codec2 700C con
saltos (218 % del canal). Para voz hay que quedarse en SF7 y comprar alcance con
potencia y antena.

El canal ocupa **439,475-439,725 MHz**, y está fuera de la banda ISM, así que
no se comparte sitio con mandos de garaje ni estaciones meteorológicas. Se
cambia en `platformio.ini` o en caliente desde la app.

## Elección de códec

Medido con lotes de 480 ms a SF7/250 kHz, contando la ocupación del canal con el
emisor y con dos repetidores más:

| Modo | 1 emisor | con 3 saltos |
|---|---|---|
| Codec2 3200 | 34 % | 101 % ✗ |
| Codec2 1600 | 19 % | 58 % |
| **Codec2 1200** | 15 % | **46 %** ✓ |
| **Codec2 700C** | 11 % | **32 %** ✓ |
| Codec2 450 | 9 % | 26 % (bajo el mínimo usable) |

En LoRa la pérdida es de **paquete entero**: un lote perdido son 480 ms de
silencio, y eso destroza la inteligibilidad mucho más que el ruido de
cuantificación. Por eso **700C mandado dos veces intercalado se entiende mejor
que 1400 mandado una vez**, con el mismo gasto de aire. Gastar el ahorro en
redundancia, no en calidad de códec.

## Formato de trama

Cabecera de 8 bytes:

```
0  magic|version (0xA1)
1  canal
2  tipo (4 bits) | saltos restantes (4 bits)
3  src[0]  \
4  src[1]   > hash de 24 bits del indicativo
5  src[2]  /
6  stream   nuevo en cada pulsación de PTT
7  seq      número de lote dentro del stream
```

Tipos: `VOZ`, `INICIO`, `FIN`, `HOLA`. El **indicativo completo solo viaja en
INICIO y HOLA** — repetirlo en cada trama de voz serían 12 % de sobre veinte
veces por segundo. La identificación de estación queda cubierta: cada pulsación
de PTT abre con INICIO y hay baliza cada 10 minutos.

Dedupe de la malla: `(src, stream, seq, tipo)`, 64 entradas, 30 s.

## Protocolo con el anfitrión (móvil o gateway)

Tramas **KISS** por USB serie y por **Bluetooth SPP** a la vez. Se eligió SPP y no
BLE: el ESP32 normal es BLE 4.2 sin Coded PHY, así que BLE no da más alcance;
SPP es un flujo de bytes y el código KISS vale tal cual; y en Android es un
socket simple, igual de 4.4 a 15.

Órdenes: `INICIO` (abre stream), `VOZ` (lote), `FIN`, `CONFIG`, `ESTADO`,
`IDENT`, `RED`, `ENLACE`, `RADIO`.
Eventos: los mismos más `HOLA`, `CANAL` (ocupado/libre), `PTT` y `LOG`.

**El códec no vive en la placa.** El nodo solo mueve bytes: el móvil codifica y
el gateway decodifica. Así se cambia de modo de Codec2 en el campo sin
reflashear, y dos estaciones con distinto ajuste se entienden porque el modo va
en la trama.

## Varios usuarios en un mismo nodo (WiFi)

El nodo puede levantar **su propio punto de acceso**: el caso de la excursión, en
el que va en la mochila y el grupo se cuelga de él sin router, sin Internet y sin
infraestructura de ninguna clase. Hasta **8 sesiones** a la vez, que es lo que
admite el AP del ESP32.

```bash
tools/nodo.py red ap PTTLoRa-EA1ABC <clave>     # y reiniciar el nodo
tools/nodo.py --tcp 192.168.4.1 --ident EA1XYZ hablar voz.wav
```

Tres reglas, y las tres importan:

- **Cada usuario emite con SU indicativo** (`IDENT`). En banda de aficionado cada
  estación se identifica; un nodo que sacara a cinco personas bajo un solo
  indicativo no sería legal. Quien no lo declare usa el del nodo.
- **Habla uno solo.** El canal es simplex y la radio, una. Al segundo que pulse
  se le contesta con `PTT` quién tiene el micrófono, para que la app pueda decir
  "habla EA3XYZ" en vez de dejar un PTT que no responde. Se suelta al soltar,
  por TOT de 3 minutos, a los 5 s sin voz o al cerrarse la conexión.
- **Los clientes de un mismo nodo se oyen entre ellos.** Por radio no podrían: el
  nodo no escucha sus propias emisiones. Así que la voz del que habla se les
  devuelve por dentro, con `rssi = 127`, un valor imposible por LoRa (allí
  siempre es negativo). Es el mismo evento de voz por los dos caminos, así que la
  app no necesita saber de esto.

La puerta de entrada: en modo AP **la clave WPA2 ya es la puerta** (y por eso se
exige, de 8 caracteres o más: un AP abierto sería dejar el mando del nodo a quien
pase por la calle). Colgado de una red ajena se pide el código de 6 cifras que
sale en la pantalla, igual que por Bluetooth.

El Bluetooth se queda en un usuario: el `BluetoothSerial` de Arduino es
monousuario por diseño. El multiusuario natural es WiFi.

## Unir dos zonas por Internet (enlace)

Dos nodos que no se oyen por radio pueden enlazarse por red, y entonces cada uno
retransmite por su antena lo que oye el otro.

```bash
tools/nodo.py enlace 203.0.113.7 4461      # y el otro nodo escucha en 4461
```

Por el enlace **no viajan órdenes, viajan tramas del aire tal cual**. El nodo del
otro lado las mete por su camino de recepción como si las hubiera oído por la
antena, y hereda gratis el descarte de duplicados, la entrega a sus clientes y la
repetición. Los bucles los corta ese mismo descarte: lo que vuelve rebotado ya
está apuntado.

Por eso el enlace tiene **su propio puerto** (4461, frente a 4460 de los
clientes): son dos protocolos distintos y distinguirlos por el contenido sería
adivinar.

### El enlace va en la CELDA, no en un nodo cliente

Medido el 8-sep-2026 y no es evidente: un nodo en `PERFIL_AUTO` **con una celda a
la vista no repite** —es lo que decide `hay_a_quien_repetir()`, y es lo que hace
que la red no se ahogue— así que lo que le entre por el enlace llega a *sus*
clientes y **nunca sale por la antena**. Con el enlace puesto en el nodo de casa,
28 lotes inyectados desde Internet llegaron a los clientes de ese nodo y los
contadores de la celda no se movieron: +2 en tres minutos, sus propias balizas.

Con el mismo enlace movido a la celda (`PERFIL_FIJO`, repite siempre), esos
mismos 28 lotes salieron al aire y otro nodo los oyó **a −69 dBm, cero
perdidos**.

O sea: **la pasarela entre Internet y la radio es infraestructura**, igual que en
TETRA. Va en la celda. Un nodo cliente con enlace no es una pasarela, es un
callejón sin salida — y uno silencioso, porque desde la app se ve entrar la voz
igual de bien.

### Redes de varias ubicaciones: el punto de reunión

Enlazar nodos entre sí funciona, pero obliga a que alguien tenga IP pública o
abra un puerto en su router, y cada nodo solo tiene cuatro ranuras. Con un punto
de reunión en un servidor se cae todo eso: **todos los nodos salen, ninguno
entra**.

```bash
tools/nodovirtual.py --escucha 4461      # en un servidor alcanzable
tools/nodo.py enlace mi-servidor.example 4461     # en cada nodo
```

`nodovirtual.py` es un nodo sin radio: reparte a todos menos al que lo trajo,
exactamente igual que un nodo con varios enlaces, y **arbitra**. Esto último es
lo que un nodo no puede hacer solo: cada nodo bloquea el PTT cuando oye el canal
ocupado, pero eso solo tapa las colisiones que le da tiempo a ver, y dos
personas en ubicaciones distintas que pulsan a la vez no se enteran la una de la
otra hasta que la voz ha cruzado. El punto de reunión sí las ve a las dos, así
que **se queda con la primera y descarta lo que le pise** hasta que termine. No
arregla el retardo — dos que pulsen dentro de un tiempo de ida se seguirán
pisando, como en cualquier repetidor — pero convierte dos transmisiones
entrelazadas e ininteligibles en una que se entiende.

Las balizas nunca se arbitran: son identificación de estación, y callarlas sería
justo lo contrario de lo que hay que hacer.

Se pueden configurar **dos destinos**, y el segundo es **respaldo, no un segundo
camino**: el nodo usa el primero mientras responda y suelta el respaldo en
cuanto el principal vuelve. Estar en los dos a la vez duplicaría el tráfico y,
peor, cada punto de reunión arbitraría por su cuenta y podrían dejar pasar
transmisiones distintas.

## Actualizar por radio (OTA)

Opcional, y **solo si le pones WiFi al nodo** — que también es opcional. Sirve
para actualizar un nodo instalado en un sitio al que no puedes subir.

```bash
espota.py -i <ip del nodo> -p 3232 -a <clave del wifi> -f firmware.bin
```

La IP sale en el estado del nodo (`wifi=…`). La clave de actualización **es la
del WiFi**, y sin ella la OTA no se levanta: en un proyecto público una clave
por defecto no es una clave.

Dos avisos: el ESP32 solo ve redes de **2,4 GHz**, y una actualización dura más
que el watchdog, así que el firmware se sale de él mientras dura — si no, la
placa se reiniciaría a medio flashear.

## Trampas que costaron tiempo

1. **`setOutputPower()` deja el SX1278 en STANDBY y no vuelve solo a recepción.**
   Cualquier nodo se quedaba sordo justo al configurarlo. Tras **cualquier**
   cambio de parámetros de radio hay que re-armar la recepción.
2. **No tocar GPIO16 en la LoRa32 v2.1.** El variant lo declara `OLED_RST` pero
   manejarlo cuelga el sistema: arranque en bucle con `TG1WDT_SYS_RESET`. No hace
   falta: `oled.begin()` reinicializa el chip por I²C.
3. **No rebautizar el Bluetooth en caliente.** `SerialBT.end()` + `begin()` es
   frágil en el ESP32 y deja la placa sin anunciarse. El nombre se fija al
   arrancar; el indicativo se guarda en NVS.
4. **Abrir el puerto serie no siempre reinicia estas placas** (CH9102). Hay que
   forzar DTR/RTS o los contadores engañan.
5. **Un solo hablante por canal.** Se escucha antes de transmitir (CAD, que
   detecta portadora LoRa bajo el ruido, donde el RSSI no serviría).
6. **La espera antes de repetir es aleatoria** (50–200 ms): si todos los
   repetidores a la vista repiten a la vez, colisionan y no llega ninguno.
7. **Mientras un nodo repite, está sordo.** Con lotes de voz cada 480 ms eso se
   come el siguiente: medido, con dos nodos y `saltos=3` se perdía el 31 % de los
   lotes. Por eso cada nodo lleva una tabla de vecinos y **solo repite si hay
   alguien a quien repetir**; con dos nodos se apaga solo y la entrega sube al
   100 %. Un nodo repetidor fijo sin móvil se compila con `-DREPETIR_SIEMPRE`,
   porque puede haber estaciones que le oigan y él no.

### `WiFiClient` no tiene `availableForWrite()`

Hereda la de `Print`, que devuelve **0 siempre**. La salida se escribía byte a
byte comprobando si había sitio, así que a los clientes por WiFi **no les llegaba
ni un byte** — y sin ningún error: transmitían perfectamente y no recibían nada.
Ahora la trama se arma entera y se manda de una, que además es lo correcto por
TCP.

Y el otro lado del mismo problema: `WiFiClient::write()` reintenta diez veces con
un `select` de un segundo, o sea **hasta diez segundos bloqueado** si un cliente
deja de leer. Eso se lleva por delante el watchdog, la radio y a todos los demás
usuarios. Se pregunta antes, con un `select` de plazo cero, y si no cabe se tira
la trama: la voz es tiempo real.

### 🔑 El socket de Android tiene que ser **inseguro**

Es la causa del fallo que tuvo bloqueada la app por Bluetooth: el `connect()`
funcionaba, el nodo veía la sesión abierta… y la app moría en el primer `read`
con `read failed, socket might closed or timeout, read ret: -1`, mientras el
nodo soltaba la ranura por `cliente Bluetooth mudo`.

`createRfcommSocketToServiceRecord()` —y también el método oculto
`createRfcommSocket(canal)`— abren un socket **seguro**: Android exige
autenticación **y cifrado** del enlace. El nodo levanta el servidor SPP con
`ESP_SPP_SEC_NONE`, que es lo que hace `BluetoothSerial::begin()` del core de
Arduino, o sea que no ofrece ninguna de las dos cosas, y el enlace se cae en
cuanto Android pide cifrar. Hay que usar
**`createInsecureRfcommSocketToServiceRecord()`**.

Por eso `tools/nodo.py` desde Linux no falla nunca: un socket RFCOMM de Python
no pide ni autenticación ni cifrado. Se puede reproducir el fallo entero sin
móvil, y es la forma de comprobar cualquier cambio de seguridad del nodo **antes**
de tocar la app:

```bash
sudo rfcomm connect       hci0 <MAC> 1   # -> Connected /dev/rfcomm0   (como la app arreglada)
sudo rfcomm -A -E connect hci0 <MAC> 1   # -> Connection reset by peer (como Android con socket seguro)
```

### Lo que hacen los firmwares que llevan años en el aire

Antes de seguir afinando a ciegas, se miró el código de los que ya funcionan:

| Proyecto | Qué es | Qué hace con el Bluetooth |
|---|---|---|
| [`sh123/esp32_loraprs`](https://github.com/sh123/esp32_loraprs) | módem KISS por LoRa para APRSdroid | `SerialBT.begin(nombre)` **y nada más**: sin PIN, sin SSP, sin plazos |
| [`richonguzman/LoRa_APRS_Tracker`](https://github.com/richonguzman/LoRa_APRS_Tracker) | tracker LoRa APRS | igual, y además `SerialBT.onData()` en vez de sondear |
| [`ge0rg/aprsdroid`](https://github.com/ge0rg/aprsdroid) | la app Android del otro lado | un hilo dueño del socket que **se reconecta solo** cada 3 s |

Tres conclusiones, y las tres se aplicaron:

1. **Nadie vigila al cliente.** La sesión SPP existe o no existe. Nuestro plazo
   de 12 segundos era invención propia y era justo lo que tiraba la app. Se
   conserva uno de **cinco minutos** —y solo para recuperar la única ranura si
   se queda colgada, que sin eso obligaría a reiniciar el nodo—, pero tan largo
   que no puede competir con el tráfico normal.
2. **Recibir por `onData()`, no sondeando.** La cola interna de
   `BluetoothSerial` son **512 bytes fijos** (`RX_QUEUE_SIZE` en el core, no hay
   forma de agrandarla) y nuestro bucle puede tardar casi un segundo en dar la
   vuelta mientras transmite: con lotes de voz cada 480 ms esa cola se llena y
   **se pierde audio sin que nada avise**. CA2RXU lo resuelve con el callback y
   lo dice en el propio comentario del código. Aquí el callback solo copia a un
   anillo de 4 kB —corre en la tarea de Bluetooth, donde no se puede transmitir
   ni tocar la NVS— y el bucle lo mastica cuando le toca. `btatasco` en el
   estado cuenta lo que no cupo: un desbordamiento silencioso se manifestaría
   como "se oye entrecortado" y no habría por dónde cogerlo.
3. **La reconexión va en la app, no en el nodo.** APRSdroid lleva años con el
   mismo patrón: un hilo que posee el socket y, ante cualquier excepción,
   espera y vuelve a abrirlo.

Y una confirmación que vale su peso: en el fuente del core,
`BluetoothSerial::begin()` arranca el servidor con
**`esp_spp_start_srv(ESP_SPP_SEC_NONE, ...)`**. Por eso el socket de Android
tiene que ser el inseguro —lo de arriba— y por eso `enableSSP()` (que pone la
máscara a `ENCRYPT|AUTHENTICATE`) es el camino si algún día se quiere seguridad
de verdad: no un PIN suelto.

### El latido tiene que caber en el plazo, con margen

El nodo suelta la ranura Bluetooth de un cliente callado. Estaba en **12 s** —para
tirar las sesiones fantasma que abre el emparejamiento de Android— y la app latía
cada **30 s**: mandaba su identificación al conectar, se callaba, y a los doce
segundos el nodo la echaba. **Siempre.** Parecía un fallo del Bluetooth de Android
y era un plazo contra otro.

Lo que faltaba era distinguir dos casos que no se parecen en nada:

| Cliente | Plazo |
|---|---|
| No ha dicho **nunca** nada → sesión fantasma | 12 s |
| Ya habló → es la app de verdad | 90 s |

Y en la app, latido cada 10 s. Entre un plazo y lo que lo alimenta conviene casi un
orden de magnitud; con 30 contra 12 no hay conexión que sobreviva.

Se comprueba sin móvil, y hay que comprobar **las dos mitades**: un cliente que
habla y luego calla 40 s debe seguir enlazado, y uno que no dice nada debe caer a
los 12 s.

### Reconectar es parte del enlace, no un extra

El Bluetooth es, con el WiFi, la única forma de hablar con el nodo. Un corte —salir
de cobertura, reiniciar el nodo, una actualización por OTA— dejaba la app muerta
hasta que el usuario entrara en los ajustes a elegir el nodo otra vez. Ahora el
servicio reintenta solo, con espera creciente de 3 a 20 s, y el estado «Sin enlace»
se puede tocar para reintentar en el acto.

Y el nodo elegido **se recuerda**: un nodo encontrado por búsqueda no queda
emparejado (el SPP no lo necesita), así que no salía en la lista de ajustes —que
solo enseñaba los emparejados— y parecía que la app no guardaba nada. Ahora sale el
primero.

### Emparejar en Android no es el código del nodo

Confunde, y mucho. El emparejamiento de los ajustes de Android es una operación
de la pila Bluetooth: **el nodo ni se entera, y no pide nada**. El código de seis
cifras lo pide el nodo al abrirse la sesión, o sea cuando conecta la app. Así que
emparejar por fuera y no ver ningún código es lo normal, no un fallo.

De hecho no hace falta emparejar en absoluto: el SPP acepta clientes sin
emparejar, y por eso la app trae su propia búsqueda.

### El aviso del código se podía perder

`onPideCodigo` se atendía solo en vivo, pero el servicio conecta por su cuenta y
el aviso llega cuando llega: si caía antes de que la pantalla estuviera
escuchando, se perdía y el usuario se quedaba con un enlace abierto que no
responde a nada, sin saber por qué. El servicio guarda que está pendiente y la
pantalla lo consulta al volver.

### El estado llegaba partido por el cable

Escribir en el serie no puede bloquear (un buffer lleno que nadie lee congelaría
el nodo), pero tirar el byte a la primera tampoco: el buffer de salida son 256 B
y la línea de estado ya pasa de eso, así que llegaba **con letras sueltas donde
debería haber campos**, con toda la pinta de ser un fallo del `snprintf` — que no
lo era. Ahora el buffer es de 1 kB y se espera hasta 15 ms a que el UART drene
antes de rendirse.

## Trabajo relacionado

- **`sh123/esp32_loradv`** (GPL-2.0) — lo más cercano que funciona: voz por LoRa
  en ESP32 con Codec2 y Opus, I2S en la placa. No tiene malla ni app. Complementario.
- **`goedzo/lilygo-t-echo-push-to-talk-lora`** (MIT) — modo PTT stub, pero buenas
  colas TX/RX y puente BLE con app.
- **QMesh** — malla inundada sincronizada con FEC Reed-Solomon+Viterbi (93 % → 99 %
  de paquetes recibidos) y códigos Walsh-Hadamard contra colisiones.
- **Meshtastic** tiene módulo de audio con Codec2 pero **solo en 2,4 GHz**: dicen
  que sub-GHz no da para audio continuo *en su malla completa*. Aquí el canal es
  dedicado, que es otra cosa.

---

## Licencia

**Apache 2.0** (ver `LICENSE` y `NOTICE`).

El códec es **[Codec2](https://github.com/drowe67/codec2)** de David Rowe,
**LGPL 2.1**. No va en este repositorio: `app/preparar.sh` lo clona de su
repositorio oficial y lo parchea para cruzarlo con el NDK.

⚠️ **La app lo enlaza estáticamente**, así que el APK que se distribuya queda
sujeto a la LGPL 2.1. Se cumple publicando el código fuente completo de la app
—que es lo que hace este repositorio— y conservando el `NOTICE`. El firmware del
nodo **no** contiene Codec2, y eso no es casualidad: el códec vive en los
extremos y la placa sólo mueve bytes. Las herramientas de `tools/` y `bench/`
llaman a `c2enc`/`c2dec` como programas externos, que no es enlazado.

## Avisos

- Esto **no es un producto**. Es un proyecto de radioaficionado y se publica por
  si le sirve a alguien, sin ninguna garantía.
- **El plan de banda es tuyo, no mío.** Lee la sección de uso legal antes de
  encender nada: 430-440 MHz está atribuida al servicio de aficionados en las
  tres regiones de la UIT, pero el reparto interno, la potencia y los usos los
  fija el plan nacional de cada país y no coinciden.
- El canal va **sin cifrar y a propósito**, porque en el servicio de aficionados
  no procede. Cualquiera con un receptor puede oírlo, y la posición que se
  publique viaja igual de clara.
