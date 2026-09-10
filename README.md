# PTT LoRa

Voz digital por LoRa en malla, para radioaficionados con licencia. Un canal, un
talkgroup, sin cifrado — como nació el LoRa APRS.

Cada nodo hace las tres cosas a la vez y sin configurar nada:

- **Puente**: si hay un móvil emparejado por Bluetooth, es su radio.
- **Repetidor**: repite lo que oye del canal, con salto menos y espera aleatoria —
  **solo si hay alguien a quien repetir**.
- **Testigo**: una placa suelta con batería ya extiende la red.

Estado, trampas aprendidas y hoja de ruta: **[HOJA-DE-RUTA.md](HOJA-DE-RUTA.md)**.

Estado: **firmware y app validados en hardware** (6-sep-2026). Voz real entre dos
nodos y entre móvil y nodo, en los dos sentidos, a 439,600 MHz con Codec2 1200.

---

## Uso legal

**Esto es para radioaficionados con licencia.** El perfil que viene de fábrica —
439,600 MHz, 17 dBm, voz sin cifrar — es legal en el servicio de aficionados y
en ningún otro. Tres cosas que conviene tener claras antes de encender:

- **La banda.** 430-440 MHz está atribuida al servicio de aficionados en las tres
  regiones de la UIT, pero el reparto interno, la potencia y los usos permitidos
  los fija **el plan nacional de cada país**, y no coinciden. Contrasta el canal
  con tu administración y con el bandplan de la IARU de tu región antes de
  transmitir. Nadie ha hecho ese trabajo por ti.
- **La identificación.** El nodo baliza tu indicativo cada minuto y lo mete en la
  cabecera de cada transmisión. Eso es identificación, y por eso el indicativo no
  es opcional: un nodo sin configurar no debe salir al aire.
- **El cifrado.** No hay, y es a propósito. Ocultar el contenido de las
  comunicaciones está prohibido en el servicio de aficionados (RR 25.2A). Quien
  quiera privacidad, este no es el proyecto.

**Fuera de la banda de aficionados esto no sirve tal cual.** En Europa, el único
hueco SRD que admite voz es 869,7-870 MHz (anexo 1, h9 de la ERC/REC 70-03):
5 mW p.r.a., ancho **≤25 kHz**, LBT y **≤1 minuto por transmisión**. El perfil
de fábrica —250 kHz y 50 mW— incumple las tres, así que no es cuestión de
cambiar la frecuencia y ya. En el resto de 863-870 MHz lo que lo impide no es
que sea voz, es el ciclo de trabajo: con el 1% habitual se transmite 36 segundos
por hora.

### Qué hace el software al respecto

El SX1278 de estas placas llega de 420 a 520 MHz, mucho más de lo que cubre
ninguna licencia de aficionado. Así que **el firmware se niega a salir de
430-440 MHz** (`BANDA_MIN`/`BANDA_MAX` en `platformio.ini`): rechaza el
`CMD_RADIO` que se vaya de banda, y si encuentra en NVS una frecuencia que no
vale —guardada por otra versión— vuelve al canal de fábrica al arrancar. La app
avisa antes de mandarlo, pero quien decide es el nodo: cualquiera puede mandar un
`CMD_RADIO` con `nodo.py` o con un script.

Esto no detiene a nadie decidido: el firmware es abierto, y cambiar esas dos
líneas y recompilar son dos minutos. No pretende otra cosa. Lo que evita es lo
único que iba a pasar de verdad — un dedo torpe escribiendo 443 en vez de 434 y
transmitiendo fuera de banda sin enterarse.

Quien tenga otra atribución, otro servicio u otro país, cambia esas dos líneas y
compila lo suyo, y con ello asume lo que emite. La potencia, en cambio, se avisa
pero no se limita: dentro de la banda es decisión del operador.

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

**439.600, y el porqué importa.** Se empezó en 434.400 y **estaba mal**: ese
canal cae en el tramo `434,000-434,594` del plan de bandas UHF de la IARU R1,
que tiene un máximo de **12 kHz de ancho de banda**. Un canal LoRa a BW 250 es
veinte veces eso, sentado encima de una veintena de canales de 12,5 kHz de otra
gente. Que el plan sea una recomendación y no una ley no lo arregla.

439.600 sale de mirar dónde deja sitio el propio plan. El bloque `438-440` está
marcado con ancho de banda **`none`** —sin límite— y dentro de él queda un hueco
sin nada asignado:

```
438,000 - 440,000   ancho: none   All mode
    438,025 - 438,175   Digital communication channels
    438,200 - 438,525   Digital communication repeater channels
    438,550 - 438,625   Multi mode
    438,650 - 439,425   Repeater output channels (7.6 MHz shift)
    ······· 439,425 - 439,800 · sin asignar ·······   <- el canal va aquí
    439,800 - 439,975   Digital communication link channels
```

A BW 250 ocupa **439,475-439,725**, con margen por los dos lados. Y de regalo
queda **fuera de la banda ISM** (433,05-434,79), donde se compartía sitio con
mandos de garaje, estaciones meteorológicas y el propio LoRa APRS.

⚠️ **Contrasta esto con TU plan nacional antes de encender.** Los planes
nacionales subdividen ese bloque de formas distintas: el del RSGB británico, por
ejemplo, usa `439,400-439,775` para salidas de repetidor de voz digital y
reserva `439,900-439,9875` para *Low Power LoRa Gateways*. Un canal de 250 kHz
se oye lejos, así que mira también el plan de tus vecinos.

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

## Hardware

LilyGO con ESP32 y SX127x. Probado en:

- **LoRa32 v2.1** (T3 v1.6.1) — entorno `lora32`
- **T-Beam v1.2** (AXP2101) — entorno `tbeam`

En la T-Beam hay que **encender ALDO2 (radio) y ALDO3 (GPS) del AXP2101** o el
SX1278 no tiene corriente y `radio.begin()` falla.

## Compilar y flashear

```bash
pio run                          # los cuatro entornos
esptool --port /dev/ttyACM0 --baud 460800 write-flash -z \
        0x10000 .pio/build/lora32/firmware.bin
```

Si cambia la tabla de particiones, flashear también `bootloader.bin` en 0x1000 y
`partitions.bin` en 0x8000.

Entornos:

| Entorno | Flash | Para qué |
|---|---|---|
| `lora32` | 37 % de 3 MB | LilyGO LoRa32 v2.1 |
| `tbeam` | 38 % | LilyGO T-Beam v1.2 |

**Un solo firmware hace todos los papeles.** Obligar a elegir binario, o a
reflashear para cambiar de función, es pedirle al usuario un conocimiento que no
tiene por qué tener. El papel se elige desde la app y se guarda en el nodo:

| Perfil | Qué hace |
|---|---|
| **Automático** (por defecto) | Puente + repetidor, callándose la repetición si no hay a quién repetir |
| **Repetidor fijo** | Repite siempre. Para un nodo en alto sin móvil: puede haber estaciones que le oigan y él no. Apaga el Bluetooth a los 5 min si nadie se conecta, con ventana de gracia en cada arranque |
| **Solo mi radio** | No repite nunca |

Se usa `huge_app.csv` porque Bluetooth Classic cuesta 828 KB y con el esquema por
defecto el firmware quedaba al 89 %.

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
```

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
