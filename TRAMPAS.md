# Trampas y cicatrices

Todo esto costó tiempo, y está escrito para que no lo cueste dos veces. No hace
falta leerlo para usar PTT LoRa: es el cuaderno de lo que salió mal, por qué, y
qué se hizo. Si vas a tocar el firmware o a escribir otro cliente, aquí está
media respuesta a los problemas que te vas a encontrar.

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


---

## De una jornada larga de campo y depuración

Todo lo de esta sección salió de usar el sistema de verdad, no de leerlo. Va
junto porque comparte una moraleja: **casi ningún síntoma apunta a su causa**.

### La copia de Internet gana siempre, y eso no es una preferencia
Con dos caminos —radio y una pasarela de Internet— la copia que llega antes es
**siempre** la de Internet: sale de una celda que ya recibió la trama entera,
mientras la de radio aún tiene que cruzar el aire, el jitter del repetidor y el
enlace con el móvil. Si el arbitraje se hace por «quién llega primero», la radio
queda de adorno **y el sistema parece ir bien justo cuando ha dejado de ir**: la
cobertura de RF se cae y nadie se entera.

La preferencia por la radio tiene que ser **estructural**, nunca una carrera. Y
va con un corolario: **no dejes que el FIN cierre la recepción**. El FIN también
viaja por los dos caminos y también se adelanta, así que vaciar al recibirlo
tira los últimos lotes de radio que venían de camino. Cierra el reloj, con un
margen.

### Un nodo no puede arbitrar entre caminos: no tiene con qué
Un intento de que el nodo eligiera «si esta transmisión me entra por la antena,
callo la de Internet» se estrelló: basta con que llegue **el INICIO** por radio
para callar el resto de la frase, y si los lotes no llegan, la voz se pierde
entera teniéndola disponible. Un nodo no tiene buffer de audio ni sabe qué
secuencias faltan. **Quien compone es el que escucha**, lote a lote. El nodo
entrega las dos copias y repite una sola vez.

### La ISR de DIO0 también salta al transmitir
En RadioLib, `setPacketReceivedAction()` deja la rutina enganchada durante la
emisión, y **DIO0 sirve para RxDone y para TxDone**. Al acabar de transmitir se
lee del FIFO un «paquete» que nadie ha recibido, con la longitud residual del
registro y el contenido de lo que se acaba de emitir. Medido: un tono de 7
tramas dejaba `tx +7` pero **`irq +15`** y `rx +0`. Se arregla descartando
`hay_paquete` justo antes de rearmar la recepción.

> **La pista que lo delata, y que es general:** una recepción de verdad **no
> puede traer una longitud incoherente**. El header explícito de LoRa lleva la
> longitud dentro y el CRC la valida. Si un paquete de 14 bytes te llega como 30,
> no ha llegado: lo estás leyendo de un FIFO que nadie ha llenado. Un `rssi` que
> parece una medida puede ser un registro sin refrescar — **antes de construir
> una teoría sobre un dato, comprueba que ese dato pueda ser lo que parece.**

### Algo se va sin decir adiós: la familia de fallo más repetida
Apareció **tres veces el mismo día en tres sitios distintos**, y merece mirarse
como familia y no como incidentes sueltos:

* un nodo que se reinicia deja su conexión TCP abierta en el servidor, que la da
  por viva — y las ranuras se van llenando de fantasmas hasta que el nodo real
  no puede entrar;
* un móvil que cambia de red deja otro socket zombi, y el reparto local le
  mandaba **su propia voz** por la conexión vieja: eco tardío;
* un cierre de transmisión encargado a un hilo que no llegó a existir dejaba el
  PTT trabado y el canal ocupado para toda la red.

**Todo lo que dependa de que otro avise necesita un plazo por debajo.** Y quien
se identifica tiene derecho a echar a su propio fantasma.

### Una baliza no debe ocupar el canal
El «canal ocupado» que bloquea el PTT se ponía con **cualquier** trama recibida,
durante 1,5 s. Una baliza dura unos 40 ms en el aire: bloqueaba el micrófono 37
veces más tiempo del que ocupaba. Ese guarda existe **por la voz** —entre lote y
lote hay huecos y no quieres declarar libre a mitad de una transmisión—; una
baliza, cuando la recibes, ya se acabó. Espaciar las balizas *no* lo arregla:
sólo reparte el mismo bloqueo en menos veces.

### El micrófono saturado y un códec malo se oyen igual
Se pueden perder horas persiguiendo el códec, el colchón de audio y las pérdidas
de radio cuando lo que pasa es que **el micro del móvil entrega de sobra y
recorta** si hablas cerca. Si la señal llega recortada del ADC, ningún control
de ganancia posterior lo deshace. Dos conclusiones prácticas: la ganancia del
micrófono tiene que poder **bajar de 1** (la nuestra sólo subía), y hay que
**medir el recorte** y decirlo — contar las muestras pegadas al techo cuesta
nada y convierte «suena mal» en un número.

### Cosas de electrónica que costaron una placa colgada
* **GPIO 6–11 son la flash interna** en un ESP32 clásico. Tocarlos cuelga la
  placa en el acto (`TG1WDT_SYS_RESET`, reinicio en bucle). El GPIO 12 (MTDI) es
  pin de arranque y tampoco se toca.
* **La OTA por cable serie corrompe.** Dos de dos intentos fallaron con error 7
  (MD5) y 0 reenvíos: KISS no lleva CRC por trama y el puerto pierde bytes con el
  nodo ocupado. Con el USB puesto, flashea por el bootloader: 11 s con
  `Hash of data verified`, y conserva la NVS. La OTA es para cuando *no* hay cable.
* **Para saber si un chip de radio está vivo**, lee su registro de identidad
  (`0x42` en un SX127x devuelve `0x12` y sólo eso). Y si no contesta, mide los
  pines como lo que son: entradas con pull-up y con pull-down. Un pin al aire
  sigue al resistor; uno sujeto por algo, no. **Compara siempre con una placa que
  funcione** — en una sana el bus está suelto y MISO en alto; con el chip sin
  alimentación, *todas* las líneas caen a masa por sus diodos de protección.
  La herramienta está en `escaner/`.

### Y dos de método
* **Un espacio invisible al final de un SSID** deja un nodo buscando para
  siempre una red que no existe, sin un solo error en ninguna parte. Recorta los
  nombres de red al recibirlos; la contraseña **no**, que ahí un espacio puede
  ser parte de la clave.
* **Arreglar el síntoma que reporta el usuario, uno detrás de otro, empeora el
  conjunto.** Tres versiones seguidas tocando el borde de la misma frase
  produjeron una involución. Cuando aparece el *segundo* síntoma del mismo
  sitio, hay que parar y mirar el camino entero. Y toda mejora que cambie cómo
  suena algo debería poder **apagarse desde los ajustes**, para poder comparar en
  el aire en vez de discutirlo.
