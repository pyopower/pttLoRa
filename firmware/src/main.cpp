// main.cpp — nodo de PTT LoRa.
//
// Cada nodo hace las tres cosas a la vez y sin configurar nada:
//   * PUENTE    : si hay un anfitrion por USB (movil o gateway), es su radio.
//   * REPETIDOR : repite todo lo que oye del canal, con saltos-- y espera
//                 aleatoria para no chocar con los otros repetidores.
//   * TESTIGO   : una placa suelta con bateria ya extiende la red.
//
// Placa: LilyGO LoRa32 v2.1 (T3 v1.6.1) — ESP32 + SX1278.
//
// TRAMPAS conocidas y por que el codigo esta asi:
//  - La inundacion multiplica el aire por el numero de repetidores a la vista.
//    Por eso los saltos van limitados y la espera antes de repetir es
//    ALEATORIA: si todos repiten a la vez, colisionan y no llega ninguno.
//  - Nunca se repite lo propio: al transmitir se mete en la cache de dedupe.
//  - Se escucha antes de transmitir (CAD del SX1278). Sin eso, dos estaciones
//    que empiezan a la vez se destruyen mutuamente la transmision entera.
//  - El repetidor entrega al anfitrion Y repite. Son cosas independientes.
//
// 🪤 TRAMPA DE FLASHEO QUE COSTO UNA HORA (8-sep-2026), APUNTADA AQUI PORQUE NO
//    SE PARECE EN NADA A SU CAUSA:
//    Esta tabla de particiones tiene DOS ranuras de aplicacion (app0 en 0x10000
//    y app1 en 0x200000) porque la actualizacion por el aire las exige. Cual de
//    las dos arranca lo decide `otadata` (0xe000).
//    **En cuanto una OTA se completa, la placa pasa a arrancar desde app1.** Y
//    a partir de ahi, `esptool write-flash 0x10000 firmware.bin` escribe en la
//    ranura que NO arranca: el flasheo dice "Hash of data verified", la placa
//    se reinicia... y sigue corriendo el firmware viejo. Sin ningun error.
//    El sintoma es de locos: compilas, flasheas, y un campo nuevo del estado
//    "no aparece" — y te pones a buscar el fallo en el codigo que si esta bien.
//    SOLUCION: flashear SIEMPRE el juego completo, que incluye
//    `0xe000 boot_app0.bin` — ese fichero deja `otadata` diciendo "arranca de
//    app0" y todo vuelve a ser predecible:
//      esptool write-flash -z 0x1000 bootloader.bin 0x8000 partitions.bin \
//                             0xe000 boot_app0.bin 0x10000 firmware.bin

#include <Arduino.h>
#include <RadioLib.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WiFiUdp.h>
#include <Update.h>
#include <esp_system.h>
#include <NimBLEDevice.h>
#include <sys/select.h>
#ifdef PLACA_TBEAM
  #define XPOWERS_CHIP_AXP2101
  #include <XPowersLib.h>
#endif
#include "protocolo.h"

// ---- patillaje de la LoRa32 v2.1 ----
#define P_SCK   5
#define P_MISO 19
#define P_MOSI 27
#define P_CS   18
#define P_RST  23
#define P_DIO0 26
#define P_DIO1 33
#ifdef PLACA_TBEAM
  #define P_LED  14        // en la T-Beam el LED es el 14, no el 25
#else
  #define P_LED  25
#endif
#define P_BAT  35        // divisor 2:1 hacia la bateria
#ifdef PLACA_TBEAM
  #define P_BOTON 38     // boton de usuario de la T-Beam
#else
  #define P_BOTON  0     // boton PRG de la LoRa32
#endif
#define P_SDA  21
#define P_SCL  22
#ifdef PLACA_TBEAM
  /* GPS de la T-Beam. El modulo (NEO-6M/8M) habla NMEA a 9600 por su propia
     UART, y su alimentacion cuelga del ALDO3 del AXP2101 — sin encender ese
     rail no hay ni un byte, igual que le pasa a la radio.
     GPIO34 es SOLO ENTRADA, que es justo lo que hace falta para recibir.
     ⚠️ GPIO12 es pin de arranque (MTDI: decide la tension de la flash). Como TX
     de una UART no molesta —se toca despues del arranque— pero NO se le puede
     poner un pull-up externo ni dejarlo alto en el reset. */
  #define P_GPS_RX 34      // entra lo que dice el GPS
  #define P_GPS_TX 12      // sale hacia el GPS (no se usa, pero la UART lo pide)
  #define GPS_BAUD 9600
#endif
// GPIO16 NO se usa: ver la nota en setup(). El variant lo llama OLED_RST pero
// en esta placa manejarlo provoca arranque en bucle.

#define SYNC_WORD      0x3B       // propio: descarta preambulos ajenos del canal
#define SALTOS_DEF     3
#define DEDUPE_N       64
#define DEDUPE_MS      30000UL
#define COLA_N         8
/* BALIZA CADA MINUTO, y no cada diez. Y el motivo no es la identificacion.
 *
 * ⚠️ LA BALIZA ES LA UNICA PRUEBA DE QUE UNA CELDA EXISTE. Se comprobo el
 * 8-sep-2026: cuando una celda repite la voz de alguien, la trama conserva el
 * `src` DEL QUE HABLO, no el suyo — asi que `apunta_vecino()` apunta al cliente
 * y **la celda no se refresca con el trafico**. Solo con sus balizas.
 *
 * Y de ahi sale el problema de verdad, que es de DISTANCIA y no de tiempo: un
 * cliente que sale de cobertura sigue creyendo que hay celda —y por tanto sigue
 * SIN REPETIR— hasta que la olvida. Con la baliza a 10 min y el olvido a 30,
 * eso son 2,5 km andando... y **45 km en coche por carretera**. O sea que justo
 * cuando hace falta la malla, esta desactivada, y nada lo explica.
 *
 * Cuesta 0,06% del canal por nodo (33 ms cada 60 s a SF7); con cinco nodos,
 * 0,28%. Nada, y compra bajar esos 45 km a 3.
 *
 * ⚠️ CON JITTER. A diez minutos daba igual, pero a uno dos nodos pueden
 * engancharse en fase y colisionar SIEMPRE — un fallo periodico y sistematico
 * que parece de cobertura. El CAD ayuda, pero lo que lo rompe es el azar. */
#define HOLA_MS         60000UL   // baliza de indicativo: 1 min
#define HOLA_JITTER_MS  15000UL   // ...mas hasta 15 s al azar, ver arriba
#define JITTER_MIN_MS  50
#define JITTER_MAX_MS  200
#define CAD_INTENTOS   12
#define OCUPADO_MS     1500UL     // sin oir nada, el canal se declara libre
#define VENTANA_BT_MS  300000UL   // ver apaga_bt_si_toca()
#define PANTALLA_MS    60000UL    // apagado de la pantalla por inactividad
/* PLAZOS DE FANTASMA — SIGUEN HACIENDO FALTA, Y ESTO SE MIDIO.
 *
 * La idea al pasar a BLE era que sobraran: el plazo de supervision del enlace
 * (los 4 s que se piden en `onConnect`) deberia detectar solo al cliente que se
 * va sin despedirse, que es justo lo que SPP no detectaba nunca.
 *
 * **MEDIDO EN BANCO EL 8-sep-2026 Y NO ES ASI.** Con el enlace abierto se tiro
 * la radio del adaptador de la raspi (`hciconfig hci0 down`, o sea sin mandar
 * ningun paquete de desconexion) y el nodo tardo **29,2 s** en soltarlo — que
 * es exactamente `BT_MUDO_NUEVO_MS`, no los 4 s de la supervision. O sea: el
 * `updateConnParams` es una PETICION y quien manda es el central; BlueZ (y
 * seguramente Android) se quedan con un plazo de supervision mucho mas largo.
 *
 * Conclusion practica: los plazos de abajo son el mecanismo de verdad, no una
 * red de seguridad. Y por eso NO pueden ser generosos: mientras hay un cliente
 * dentro el nodo deja de anunciarse, asi que un fantasma es un nodo invisible.
 * Se ajustan a lo que hace la app (un latido cada 10 s) con margen de sobra. */
/* Un cliente que no dice NADA en este tiempo pierde la ranura.
 *
 * Era un minuto, y ese minuto era el problema: **el emparejamiento de Android
 * abre y cierra una sesion SPP por su cuenta**, sin mandar nada. El ESP32 solo
 * admite UN cliente, asi que esa sesion fantasma dejaba fuera a la app durante
 * un minuto entero — con el sintoma de "read failed, socket might closed" en el
 * movil y un codigo de acceso generado para un cliente que no existia.
 * Doce segundos: una app de verdad manda su identificacion nada mas conectar,
 * asi que si en doce segundos no ha dicho ni mu, no es un cliente. */
/* DOS PLAZOS, y la diferencia entre ellos era el fallo:
 *  - `BT_MUDO_NUEVO_MS`: el cliente **no ha dicho NUNCA nada**. Eso es la
 *    sesion fantasma de arriba, y a los 12 s se tira.
 *  - `BT_MUDO_MS`: el cliente **ya hablo**, o sea que es la app de verdad. Con
 *    12 s tambien para estos, **la app se caia sola cada vez**: manda su
 *    identificacion al conectar y luego un latido, y si el latido tarda mas que
 *    el plazo el nodo le cierra la puerta a un cliente que esta perfectamente.
 *    Sintoma: "conecta y a los pocos segundos se desconecta".
 *    Noventa segundos dan margen a cualquier latido razonable y siguen soltando
 *    la ranura de un movil que se fue de cobertura. */
#define BT_MUDO_NUEVO_MS 30000UL
/* UN MINUTO: seis veces el latido de la app (10 s), que es margen de sobra, y
 * a la vez lo bastante corto como para que una ranura colgada no tenga a nadie
 * esperando. Estuvo en 12 s —que competia con el trafico normal y echaba a la
 * app cada vez— y en 5 min, que era el otro extremo: un movil que se va de
 * cobertura sin cerrar dejaba el nodo inaccesible un buen rato.
 * Los firmwares de referencia (esp32_loraprs, el LoRa APRS Tracker de CA2RXU)
 * no vigilan al cliente en absoluto. Aqui hace falta, y con BLE tambien: ver la
 * medicion de arriba.
 * **45 s**, o sea CUATRO latidos perdidos de la app. Estuvo en 12 s (competia
 * con el trafico normal y echaba a la app cada vez), en 60 s y en 3 min. Tres
 * minutos era demasiado precisamente porque el nodo no se anuncia mientras cree
 * tener un cliente: alguien que sale de cobertura y vuelve se encontraria el
 * nodo invisible durante tres minutos. */
#define BT_MUDO_MS       45000UL
// Un nodo desatendido que se cuelga es PEOR que no tenerlo: nadie se entera y
// los demas siguen contando con el. De ahi las tres salvaguardas de abajo.
#define WDT_S          10         // el bucle nunca tarda tanto: 12 intentos de
                                  // CAD (~0,7 s) mas la transmision (~0,4 s)
#define RX_MUDO_MS     600000UL   // 10 min sin oir NADA -> rearmar la recepcion
#define REINICIO_MS    86400000UL // 24 h -> reinicio preventivo (solo PERFIL_FIJO)
#define VECINOS_N      16
#define VECINO_MS      1800000UL  // 30 min: para la LISTA de vecinos (`vec=` y
                                  // lo que enseña la app). Aqui recordar de mas
                                  // no hace daño.
/* CUANTO SE RECUERDA UNA CELDA — Y ESTO NO ES LO MISMO QUE LO DE ARRIBA.
 *
 * Los dos usos de la tabla de vecinos compartian el plazo de 30 min y son cosas
 * distintas: "¿quien anda por aqui?" es informativo, pero **"¿hay celda?" es una
 * decision operativa** — de ella depende que este nodo repita o se calle.
 *
 * Y los dos errores posibles NO cuestan lo mismo:
 *   - repetir creyendo que no hay celda, habiendola: cuesta un 15% de canal.
 *   - NO repetir creyendo que hay celda, no habiendola: **no hay comunicacion**.
 * Con esa asimetria hay que equivocarse REPITIENDO, o sea plazo corto.
 *
 * 2,5 balizas: aguanta perder una (que era el motivo del plazo largo) y aun asi
 * reacciona en dos minutos y medio — 170 m andando, 3 km en carretera. */
#define CELDA_MS        150000UL  // 2,5 balizas

SX1278 radio = new Module(P_CS, P_DIO0, P_RST, P_DIO1);
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
static bool hay_oled = false;

/* BLUETOOTH DE BAJA ENERGIA (BLE), no Bluetooth Classic.
 *
 * ⚠️ CAMBIO DE FONDO EN LA v1.26, Y EL MOTIVO IMPORTA.
 *
 * Hasta la v1.25 esto era `BluetoothSerial` (SPP). Sobre el papel SPP era la
 * eleccion obvia —es un flujo de bytes y el codigo KISS vale tal cual, sin
 * trocear ni reensamblar— y asi estuvo escrito aqui durante quince versiones.
 * En la practica fue una noche entera de averias encadenadas:
 *   - **La pila de Espressif se suicida sin memoria.** `btc_spp_write` reserva
 *     un buffer, NO comprueba la reserva y encola un puntero nulo:
 *     `assert failed: fixed_queue_enqueue (data != NULL)` -> panico ->
 *     reinicio -> el movil reconecta -> vuelve a pasar. Desde fuera parece
 *     "la app no conecta". El apaño (una guarda de heap minima) fue peor que
 *     la enfermedad: dejaba el enlace abierto y MUDO.
 *   - **Una sola ranura de cliente.** El ESP32 admite UN cliente SPP, y no se
 *     entera de que se ha ido si se va sin cerrar (fuera de cobertura, app
 *     matada). De ahi todo el andamiaje de fantasmas, plazos de mudez y
 *     expulsiones —`bt_expulsa`, `BT_MUDO_MS`, `BT_CEDE_MS`— que era
 *     complejidad pura para tapar una limitacion del tubo.
 *   - **El emparejamiento de Android abria una sesion SPP suya**, sin mandar
 *     nada, y esa sesion fantasma dejaba a la app fuera durante un minuto.
 *   - **La seguridad no cuajaba**: el SPP acepta clientes sin emparejar, y al
 *     exigir autenticacion no entraba nadie, tampoco el dueño.
 *   - Y encima 828 kB de flash y ~90 kB de heap para el controlador Classic.
 *
 * BLE resuelve las cinco cosas: NimBLE es mucho mas ligero, la desconexion se
 * detecta de verdad (plazo de supervision de 4 s, no "a ver si dice algo"),
 * admite varios clientes, y la seguridad va por passkey del propio BLE cuando
 * la queramos.
 *
 * **EL PROTOCOLO KISS NO CAMBIA. Solo cambia el tubo.** Lo unico que hay que
 * hacer de mas es trocear al escribir (una notificacion no puede pasar de
 * MTU-3 bytes); al leer da igual, porque KISS es un flujo de bytes y el
 * desentramado ya reensambla solo.
 *
 * Servicio: el "Nordic UART" (NUS), que es el de facto para esto y el que
 * usan Meshtastic y media docena de firmwares mas — asi cualquier terminal BLE
 * de la tienda sirve para depurar.
 *   RX (el movil ESCRIBE aqui, sin respuesta) y TX (el nodo NOTIFICA).
 * Los nombres son desde el punto de vista del MOVIL, que es la convencion de
 * Nordic; no confundirlos es la mitad de los fallos de un port de esto. */
#define UUID_NUS_SERV "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define UUID_NUS_RX   "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // movil -> nodo
#define UUID_NUS_TX   "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // nodo -> movil

static NimBLEServer         *ble_srv = nullptr;
static NimBLECharacteristic *ble_tx  = nullptr;
/* Handle de la conexion, o 0xFFFF si no hay ninguna. Es lo que hace falta para
   pedir parametros de conexion y para desconectar a mano. */
static volatile uint16_t ble_conn = 0xFFFF;
/* MTU negociado. 23 es el minimo del estandar y lo que hay hasta que el movil
   pide mas; con la cabecera ATT (3 B) eso deja 20 bytes utiles por
   notificacion. Android suele subir a 517. */
static volatile uint16_t ble_mtu = 23;
/* ¿Ha activado el movil las notificaciones? Un cliente puede conectar, leer el
   GATT y no suscribirse nunca; notificarle entonces es tirar bytes. Lo pone
   `CbTxBLE::onSubscribe` cuando el movil escribe su CCCD. */
static volatile bool ble_suscrito = false;
/* ⚠️ `CbTxBLE::onStatus` llega TARDE y en otra tarea (evento
   `BLE_GAP_EVENT_NOTIFY_TX` de NimBLE), no dentro de `notify()`. Por eso no
   sirve para decidir si reintentar —ver la nota de `ble_escribe`— y lo unico
   que se hace con el es contar los fallos. */
static bool bt_conectado = false;
static bool bt_activo = false;
/* AUTORIZACION DE LA CONEXION BLUETOOTH.
 *
 * Sin esto, cualquiera al alcance se conecta al nodo, transmite con tu
 * indicativo y hasta le mete un WiFi propio para luego reprogramarlo por OTA.
 * Comprobado: `enableSSP()` NO basta, el SPP acepta clientes sin emparejar.
 *
 * Por eso la puerta esta en el protocolo: al abrirse una conexion el nodo
 * genera un codigo de 6 cifras, lo enseña EN SU PANTALLA (nunca por el propio
 * Bluetooth, que seria como dar la llave por debajo de la puerta) y hasta que
 * no le llega ese codigo ignora cualquier otra orden.
 *
 * El movil que acierta queda apuntado y las siguientes veces entra solo. Por
 * USB no se pide nada: quien tiene el cable ya tiene el nodo en la mano.
 */
static volatile uint32_t bt_codigo = 0;
/* Movil autorizado pendiente de guardar en la NVS: ver la nota de CMD_CONFIRMA. */
static uint8_t  bt_por_apuntar[6];
static bool     hay_que_apuntar = false;
static bool     bt_autorizado = false;
static volatile bool bt_nuevo = false;   // conexion recien abierta, por resolver
static volatile bool bt_cerrado = false; // ...y recien cerrada
static uint32_t bt_ultimo = 0;
static bool     bt_hablo = false;  // el cliente ha dicho algo alguna vez

/* AMPLIFICADOR EXTERNO: linea de PTT y preambulo.
 *
 * Un amplificador que conmuta por deteccion de RF (VOX) tiene que despertar
 * ANTES de que llegue lo que hay que amplificar, y en LoRa lo primero que
 * llega es el preambulo: a SF7/BW250 dura **6,3 ms**. Si el amplificador tarda
 * mas que eso, se come el preambulo y **el paquete entero se pierde** — no
 * llega degradado, no llega. (En DMR esto no se nota: la emision es continua y
 * hay sincronismo cada 30 ms, asi que perder los primeros milisegundos es
 * invisible. En LoRa cada paquete es independiente y el preambulo es la unica
 * oportunidad de detectarlo. Por eso un amplificador que va bien en DMR puede
 * fallar aqui.)
 * Dos remedios, y estan los dos:
 *  - `pin_ptt`: un GPIO en alto mientras se transmite, con `ptt_previo` ms de
 *    adelanto. Es lo correcto si el amplificador tiene entrada de PTT.
 *  - `preambulo`: alargarlo da mas margen al VOX a costa de aire (cada simbolo
 *    son 0,51 ms a SF7/BW250; 1,02 ms a SF8).
 *    ⚠️ **HAY QUE PONER EL MISMO EN TODOS LOS NODOS.** El SX127x usa el
 *    registro de preambulo tambien al RECIBIR: medido, con el emisor en 32 y
 *    el receptor en 8 **no llega ni un paquete**, y al igualarlos vuelve a
 *    llegar todo. O sea que es parametro de canal, como el SF, y su coste en
 *    aire lo pagan todos — no solo el nodo que lleva amplificador. */
static uint8_t  pin_ptt     = 0;     // 0 = sin linea de PTT
static uint8_t  ptt_previo  = 10;    // ms de adelanto antes de emitir
static uint8_t  ptt_cola    = 5;     // ms que sigue en alto despues
static uint8_t  preambulo   = 8;     // simbolos

/* Que hacer con el Bluetooth. Ver CMD_BT en protocolo.h para el porque de que
   sea un ajuste propio y no algo derivado del perfil.
   Por defecto **SIEMPRE ENCENDIDO**: que un nodo se vuelva inalcanzable solo
   porque alguien le puso perfil de repetidor es justo el fallo que se quiere
   evitar. Quien quiera ahorrar, que lo diga. */
#define BT_SIEMPRE  2
#define BT_VENTANA  1
#define BT_APAGADO  0
static uint8_t  bt_modo = BT_SIEMPRE;

/* RECEPCION BLUETOOTH POR CALLBACK, NO SONDEANDO.
 *
 * En BLE no hay cola interna que valga: lo que llega escrito en la
 * caracteristica RX se entrega en un callback y si no lo copias, se pierde.
 * Y nuestro bucle principal puede tardar cerca de un segundo en dar la vuelta
 * cuando transmite (doce intentos de CAD mas la emision), asi que sondear no
 * es opcion: con lotes de voz cada 480 ms **se perderia audio del movil sin
 * que nada avise**.
 *
 * La vuelta de tuerca es la de siempre y sigue siendo obligatoria: **el
 * callback corre en la tarea de la pila de Bluetooth**, donde no se puede
 * transmitir por radio ni tocar la NVS, asi que solo copia al anillo y el
 * bucle principal lo mastica cuando le toca. Un anillo de un solo productor y
 * un solo consumidor no necesita cerrojo.
 *
 * `bt_atasco` cuenta los bytes que no cupieron: sin contador, un
 * desbordamiento es un fallo invisible que se manifiesta como "se oye
 * entrecortado". */
/* 4 kB. Eran 2 kB apretando con Bluetooth Classic comiendose 90 kB de heap;
   sin el sobra memoria y este es justo el sitio donde gastarla: por aqui pasan
   los trozos de una actualizacion de firmware. */
#define BT_ANILLO 4096
static uint8_t  bt_anillo[BT_ANILLO];
static volatile uint16_t bt_cabeza = 0, bt_cola = 0;
static uint32_t bt_atasco = 0;
/* Notificaciones que no se pudieron entregar. **YA NO ES UNA GUARDA DE HEAP.**
   Con SPP habia que comprobar la memoria libre ANTES de escribir, porque la
   pila de Espressif se mataba a si misma si le fallaba el malloc (ver la nota
   de arriba). Aquella guarda, mal calibrada, dejo el enlace mudo una noche
   entera. NimBLE no revienta: si no hay hueco para la notificacion, `notify()`
   devuelve false y ya. Asi que se intenta, se reintenta un poco, y si aun asi
   no cuela se cuenta aqui — sin decidir por adelantado que el nodo esta mal. */
static uint32_t bt_saltadas = 0;
/* Resultado de arrancar el anuncio BLE. Ver `visible_bt()`. */
static int      bt_visible = -1;

/* Escribe por BLE troceando al MTU. Es LO UNICO que BLE obliga a hacer de mas
   que SPP.
   Un lote de voz son 74 bytes y una linea de estado ~450: con el MTU minimo de
   23 (20 utiles) el estado sale en 23 notificaciones, y con el MTU que negocia
   Android (517) en una sola. Da igual como salga: el otro extremo desentrama
   KISS sobre un flujo, no sobre paquetes.
   El reintento no es adorno: NimBLE tiene un numero fijo de buffers de salida y
   al mandar varias notificaciones seguidas se agotan. Dos milisegundos bastan
   para que la radio BLE drene uno. Diez intentos = 20 ms, muy por debajo del
   watchdog de 10 s. */
static bool ble_escribe(const uint8_t *d, size_t n)
{
    if (!ble_tx || ble_conn == 0xFFFF || !ble_suscrito) return false;
    /* MTU-3: los tres bytes son la cabecera ATT de la notificacion. Con el MTU
       minimo del estandar (23) quedan 20 utiles; Android negocia 517 y entonces
       casi todo cabe en una sola. El tope de 244 es por prudencia: algunas
       pilas se atragantan con notificaciones mayores aunque el MTU las permita,
       y trocear de mas no cuesta nada porque el otro extremo desentrama KISS
       sobre un flujo de bytes, no sobre paquetes. */
    size_t trozo = (ble_mtu > 3) ? (size_t)(ble_mtu - 3) : 20;
    if (trozo > 244) trozo = 244;
    for (size_t i = 0; i < n; i += trozo) {
        size_t k = n - i;
        if (k > trozo) k = trozo;
        /* ⚠️ AQUI NO SE PUEDE REINTENTAR, Y CONVIENE SABER POR QUE.
         *
         * Hubo una version de esto con un bucle de reintento que miraba
         * `ble_estado_notif` justo despues de `notify()`. **Era codigo muerto**:
         * en NimBLE 1.4 `notify()` no devuelve nada y no llama a `onStatus`;
         * quien lo llama es el evento `BLE_GAP_EVENT_NOTIFY_TX`, que llega
         * DESPUES y en la tarea de NimBLE. O sea que la comprobacion siempre
         * daba "bien" y el reintento no entraba jamas.
         * Se quita en vez de arreglarlo: para reintentar de verdad habria que
         * esperar el evento, y esperar dentro del bucle principal es
         * exactamente lo que no se puede hacer aqui (por aqui pasa el audio).
         * `notify()` es "mandar y olvidarse": si no cabe, se pierde.
         *
         * Lo que si vale es CONTARLO: `bt_saltadas` (el campo `btsalta=` del
         * estado) lo lleva `CbTxBLE::onStatus` cuando el fallo llega. Si ese
         * numero sube, es que hay que trocear mas fino o pedir menos ritmo.
         * Y para lo que de verdad no puede perder nada —la actualizacion de
         * firmware— el remedio esta en el otro extremo: el movil escribe **con
         * respuesta**, que si tiene control de flujo. */
        ble_tx->notify(d + i, k);
    }
    return true;
}

static void bt_datos(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint16_t sig = (uint16_t)((bt_cabeza + 1) % BT_ANILLO);
        if (sig == bt_cola) { bt_atasco += (uint32_t)(n - i); return; }
        bt_anillo[bt_cabeza] = d[i];
        bt_cabeza = sig;
    }
}
static bool     ota_curso = false;
static uint32_t ota_bytes = 0, ota_total = 0, ota_visto = 0;
static uint8_t  pantalla_modo_previo = PANTALLA_AUTO;
static uint16_t ota_idx = 0;          // trozo que toca escribir
#define OTA_MUDO_MS 15000UL           // ultimo byte recibido por Bluetooth
static uint8_t  bt_par[6];               // BDA del que esta conectado
#define CONOCIDOS_N 4
static char nombre_bt[32] = "off";
static bool bt_ok = false;
static char bt_mac[20] = "?";
#ifdef PLACA_TBEAM
XPowersPMU pmu;
static bool hay_pmu = false;
#endif

// ---- configuracion viva ----
static char     mi_indicativo[MAX_INDICATIVO + 1] = "NOCALL";
/* NOMBRE DEL NODO, aparte del indicativo.
 *
 * El nombre Bluetooth se hacia con el indicativo y era un lio: el indicativo
 * cambia (C31AG, C31AG-1, C31AG-2 segun quien lo tocara), asi que en la lista
 * del movil los nodos bailaban de nombre y no habia forma de saber cual era
 * cual. Son dos cosas distintas y ahora se guardan aparte:
 *   - el INDICATIVO identifica la ESTACION por radio (obligacion legal),
 *   - el NOMBRE identifica el CACHARRO en la lista del movil, y no cambia.
 * Si no se pone ninguno se usa `nodo-XXXX`, los cuatro ultimos digitos de la
 * MAC: distinto para cada placa y estable de por vida. */
static char     nombre_nodo[17] = "";
static uint32_t mi_src   = 0;

/* ---- DONDE ESTA ESTE NODO ----
 *
 * Enteros en diezmillonesimas de grado, como viajan en la baliza: asi no hay
 * conversion en el camino caliente ni coma flotante en la trama.
 *
 * Por que hay dos casos y no uno: ver la nota de CMD_POS en protocolo.h. En
 * resumen, una posicion VIVA (del movil o del GPS de la placa) vive en RAM y
 * caduca; una posicion FIJA (la de una celda) vive en NVS y no caduca.
 *
 * `pos_publica` es lo que decide si la posicion SALE POR LA ANTENA. Va aparte a
 * proposito: la posicion por RF viaja en claro y la repite media red, asi que
 * "no publicar" tiene que poder ser de verdad — el nodo la sabe, la usa para
 * lo suyo, y no la cuenta. Hoy es siempre 1 salvo que se apague; la eleccion
 * por usuario (ciclico / solo al transmitir / nunca) es de la fase 3. */
static int32_t  pos_lat = 0, pos_lon = 0;
static bool     hay_pos = false;
static uint8_t  pos_origen = POS_VIVA;
static bool     pos_publica = true;
static uint32_t t_pos = 0;            // cuando se supo (millis)
/* Media hora. Es mucho mas que el intervalo con el que un movil manda su GPS
   (segundos) y mucho menos que un paseo: si se pasa de ahi, o el movil se fue o
   el GPS no ve el cielo, y en los dos casos la ultima posicion ya no describe
   donde esta el nodo. Mejor callar que mentir en un mapa. */
#define POS_VIVA_MS   1800000UL
/* Cuanto se le concede al GPS propio la ultima palabra frente a un movil. Dos
   minutos: mas que de sobra para un receptor que da posicion cada segundo, y
   poco para que, si pierde el cielo, el movil pueda tomar el relevo. */
#define GPS_MANDA_MS   120000UL

static inline bool pos_vale()
{
    if (!hay_pos) return false;
    if (pos_origen == POS_FIJA) return true;
    return (uint32_t)(millis() - t_pos) < POS_VIVA_MS;
}

/* IDENTIFICADOR DE ESTE NODO EN LA MALLA — SALE DE LA MAC, NO DEL INDICATIVO.
 *
 * ⚠️ ESTO ERA UN HASH DEL INDICATIVO Y ROMPIA LA RED EN SILENCIO. Visto en
 * vivo el 8-sep-2026: la app manda su indicativo al nodo al guardar ajustes
 * (es su comportamiento normal), asi que **los dos nodos del mismo dueño
 * acabaron llamandose `C31AG` los dos**. Mismo indicativo -> mismo hash ->
 * mismo `src`. Y a partir de ahi:
 *   - `hay_celda_a_la_vista()` descarta los vecinos cuyo `src` es el propio,
 *     asi que **el cliente tomo la baliza de la celda por suya y la ignoro**:
 *     `modo=malla` con una celda a diez metros.
 *   - `src != mi_src` (no repetir lo propio) tampoco distingue, asi que un
 *     nodo deja de repetir lo del otro.
 * O sea que la arquitectura entera se cae, sin un solo error por ningun lado.
 *
 * El indicativo es de quien OPERA y por diseño se repite entre los cacharros de
 * la misma persona; el `src` tiene que identificar al CACHARRO. Son cosas
 * distintas y ahora salen de sitios distintos: el `src` de la MAC (unica de
 * fabrica) y el indicativo sigue viajando en INICIO y HOLA, que es de donde los
 * demas aprenden quien habla.
 *
 * Compatible con firmwares viejos: cada nodo pone SU `src` en lo que emite y el
 * receptor usa el que venga en la trama, sin recalcularlo. */
static uint32_t src_de_la_mac()
{
    uint8_t m[6];
    if (esp_read_mac(m, ESP_MAC_BT) != ESP_OK) return 0;
    /* Los tres ultimos bytes ya son el numero de serie del chip: con eso el
       reparto es uniforme y no hace falta mezclar nada. */
    return ((uint32_t)m[3] << 16) | ((uint32_t)m[4] << 8) | m[5];
}
static uint8_t  mi_canal = 1;
static uint8_t  saltos_def = SALTOS_DEF;
static uint8_t  potencia = CANAL_POTENCIA;
static uint8_t  perfil = PERFIL_AUTO;
// Ajustes de radio en caliente. Antes eran constantes de compilacion; ahora se
// cambian desde la app y se guardan, que es lo que hace falta para tener
// canales de verdad y no un binario por frecuencia.
static float    frecuencia = CANAL_MHZ;
static float    ancho = CANAL_BW_KHZ;
static uint8_t  sf = CANAL_SF, cr = CANAL_CR;
static uint8_t  pantalla_modo = PANTALLA_AUTO;
/* ¿Se pide el codigo de 6 cifras al abrir sesion?
 *
 * DE MOMENTO NO, por decision del usuario: mientras la app no consiga enseñar
 * la peticion, el codigo no protege nada — solo impide usar el nodo. Un
 * candado que deja fuera al dueño y a nadie mas no es seguridad.
 * El mecanismo se queda entero y probado; esto solo decide si se exige. Hay que
 * volver a ponerlo en cuanto la app lo muestre de forma fiable, porque sin el
 * cualquiera al alcance transmite con tu indicativo y puede reconfigurar el
 * nodo (ver la seccion de la autorizacion, mas arriba). */
static bool     pedir_codigo = false;
static String   wifi_ssid, wifi_clave;
/* SEGUNDA RED, y no es un lujo: el nodo que va a un punto alto se configura
   AQUI, con la red de casa, y tiene que arrancar ALLI, con otra. Sin una
   segunda red hay que elegir: o se puede probar antes de subirlo, o funciona
   cuando sube. Con las dos guardadas se alterna entre ellas hasta que alguna
   conteste, asi que la misma placa vale en los dos sitios sin tocarla — que
   importa mucho cuando ademas no tiene Bluetooth con el que rescatarla. */
static String   wifi_ssid2, wifi_clave2;
static uint8_t  wifi_cual = 0;        // cual se esta probando: 0 o 1
static uint32_t wifi_probando = 0;    // desde cuando
static bool     wifi_activo = false, ota_lista = false;
/* Modo de red del nodo. RED_OFF por defecto: la radio de 2,4 GHz apagada es
   ~80 mA menos y una superficie de ataque menos en un aparato que por lo demas
   no necesita red para nada. */
static uint8_t  red_modo = RED_OFF;
/* Enlaces SALIENTES hacia otros nodos por Internet.
   Dos destinos como mucho, para que queden ranuras libres para los entrantes:
   la topologia tipica es un nodo con IP alcanzable al que llaman los demas, y
   la util de verdad es la cadena (A->B->C), donde el del medio tiene uno de
   cada. */
#define SALIENTES_N   2
static String   enlace_host[SALIENTES_N];
static uint16_t enlace_puerto[SALIENTES_N] = {0};
static uint32_t t_enlace = 0;

/* CANAL DE MANDO SALIENTE.
 *
 * Un nodo en un sitio prestado —el caso del tejado— esta en una red donde NO
 * se pueden abrir puertos. Y todo lo que sirve para administrarlo va hacia
 * dentro: la consulta de estado y la configuracion (4460) y la OTA de Arduino
 * (3232). Instalado alli, ese nodo quedaria incomunicado salvo bajandolo, que
 * es justo lo que no se puede hacer con comodidad.
 * La vuelta es que **llame el**: abre una conexion saliente contra un relevo
 * nuestro y se comporta como si por ahi hubiera entrado un cliente normal. No
 * hace falta protocolo nuevo, porque el que ya hay lleva estado,
 * configuracion y **actualizacion de firmware propia** (`CMD_OTA_*`), y
 * funciona por cualquier tubo.
 * Ocupa una ranura de cliente TCP, no de enlace: por el van ORDENES, no tramas
 * del aire. Son dos protocolos distintos y confundirlos seria adivinar. */
static String   mando_host;
static uint16_t mando_puerto = 0;
static uint32_t t_mando = 0;
static int8_t   mando_tubo = -1;

// ---- estado ----
static volatile bool hay_paquete = false;
static uint8_t  tx_stream = 0, tx_seq = 0;
static bool     transmitiendo = false;
static uint32_t t_ultimo_rx = 0, t_ultima_hola = 0;
/* Cuanto se espera hasta la PROXIMA baliza. Variable y no constante porque
   lleva el jitter dentro: ver `manda_hola()` y, sobre todo, la cicatriz de
   abajo sobre por que el jitter NO puede ir sumado a `t_ultima_hola`. */
static uint32_t hola_espera = HOLA_MS;
static bool     canal_ocupado = false;
static bool     hay_anfitrion = false;
static uint32_t n_rx = 0, n_tx = 0, n_repetidas = 0, n_dup = 0, n_malas = 0;
static uint32_t n_calladas = 0;      // repeticiones que NO hizo falta hacer
static char     ultimo_ind[MAX_INDICATIVO + 1] = "";
static int      ultimo_rssi = 0;
static uint32_t t_pantalla = 0;
static bool     redibujar = true;
static uint32_t t_actividad = 0;      // ultimo suceso que merece encender
static bool     pantalla_on = true;

/* Estaciones oidas ultimamente. Sirven para decidir si repetir tiene sentido:
   ver `hay_a_quien_repetir()`. Se apuntan con CUALQUIER trama, no solo con las
   balizas, porque cualquier cosa que oigamos demuestra que ese nodo esta ahi. */
struct Vecino { uint32_t src; uint32_t t; bool celda; };
static Vecino vecinos[VECINOS_N];

struct Vista { uint32_t src; uint8_t stream, seq, tipo; uint32_t t; };
static Vista dedupe[DEDUPE_N];
static uint8_t dedupe_i = 0;

struct Pendiente { uint8_t buf[CAB_LEN + MAX_PAYLOAD]; uint8_t len; uint32_t cuando; };
static Pendiente cola[COLA_N];
static uint8_t cola_n = 0;

/* ------------------------------------------------------------------ TUBOS --
 *
 * Un "tubo" es cualquier camino por el que un anfitrion habla con el nodo: el
 * cable USB, el Bluetooth, o un cliente por WiFi. Antes habia dos, fijos, con
 * el estado repartido en variables sueltas (`tubo_usb`, `tubo_bt`, un `bool
 * por_bt`...). Eso valia mientras el nodo diera servicio a UNA persona.
 *
 * El multiusuario obliga a generalizarlo, y de la tabla salen casi gratis las
 * tres cosas que hacian falta:
 *   - cada tubo lleva SU indicativo, asi que cada usuario emite con el suyo
 *     (requisito expreso: en radio de aficionado no se emite con indicativo
 *     ajeno);
 *   - se puede saber quien tiene el PTT y negarselo a los demas;
 *   - se puede devolver el audio del que habla a los OTROS clientes del mismo
 *     nodo, que por radio no se oirian entre ellos (el nodo no escucha sus
 *     propias emisiones).
 */
enum : uint8_t { TUBO_LIBRE = 0, TUBO_USB, TUBO_BT, TUBO_TCP, TUBO_ENLACE };

/* El buffer de desentramado del cable y del Bluetooth es grande porque por ahi
   pasan los trozos del firmware en una actualizacion; los clientes de WiFi solo
   mueven voz (una trama de radio no llega a 210 bytes), asi que con 320 van
   sobrados y nos ahorramos 3 kB de RAM que el WiFi necesita mas que ellos. */
#define KISS_MAX      1100
/* Peor caso: una trama de radio llena (8 + MAX_PAYLOAD = 208 B) en la que
   TODOS los bytes sean 0xC0 o 0xDB y haya que escaparlos = 416, mas los dos
   delimitadores. El audio comprimido es binario, asi que ese caso no es
   teorico. Con 320 se truncaban en silencio las tramas grandes. */
#define KISS_MAX_TCP  460
/* Ocho, que es lo que admite el punto de acceso del ESP32 con holgura: tener
   mas sitio en la red que sesiones solo crearia la situacion absurda de estar
   conectado al WiFi del nodo y que te rechace la sesion. El limite de verdad no
   es este numero, es el TURNO DE PALABRA: solo habla uno, asi que tres usuarios
   o diez ocupan el aire exactamente igual. */
#define TCP_N         4          // clientes por WiFi a la vez.
                                  // Eran 8 y cada ranura cuesta 460 B de
                                  // buffer: con la pila Bluetooth comiendose
                                  // 90 kB, la memoria es el recurso escaso y
                                  // cuatro clientes a la vez ya es un grupo.
/* Enlaces con OTROS NODOS por Internet. Varios, no uno, y esa es toda la
   diferencia entre una pareja de nodos y una red de ubicaciones: con una sola
   ranura, un nodo intermedio de A-B-C tendria el enlace entrante de A ocupandola
   y no podria salir hacia C. Cada ranura puede ser saliente (host configurado)
   o entrante, indistintamente. */
#define ENLACES_N     4
#define TUBOS_N       (2 + TCP_N + ENLACES_N)

struct Tubo {
    uint8_t   clase = TUBO_LIBRE;
    bool      autorizado = false;
    char      indicativo[MAX_INDICATIVO + 1] = "";
    uint32_t  src = 0;             // hash del indicativo de ESTE cliente
    WiFiClient cli;
    uint8_t  *buf = nullptr;
    uint16_t  cap = 0, n = 0;
    bool      dentro = false, escape = false;
    uint32_t  t_ultimo = 0;
    int8_t    salida_k = -1;   // enlace: destino saliente que ocupa; -1 = entrante
};

static uint8_t buf_usb[KISS_MAX], buf_bt[KISS_MAX];
static uint8_t buf_tcp[TCP_N + ENLACES_N][KISS_MAX_TCP];
static Tubo tubos[TUBOS_N];
#define T_USB     0
#define T_BT      1
#define T_TCP0    2
#define T_ENL0    (2 + TCP_N)                 // primera ranura de enlace
#define T_ENL     T_ENL0                       // (el primero, para lo que mire uno solo)

/* Arbitraje del microfono. Con varios usuarios colgados del mismo nodo solo
   puede hablar uno: el canal es simplex y el nodo tiene una sola radio. El que
   llega primero se lo queda; a los demas se les dice quien esta hablando para
   que la app pueda pintarlo y bloquear el PTT en vez de dejar al usuario
   apretando un boton que no hace nada. */
static int8_t   ptt_de = -1;
static uint32_t t_ptt = 0;
#define PTT_MAX_MS  180000UL      // TOT del nodo: 3 min, como la app PTT
#define PTT_MUDO_MS 5000UL        // sin voz ni FIN, se da por soltado

static void suelta_ptt(const char *motivo);
static uint8_t cuenta_clientes();
static void cierra_tubo(int idx);
static void del_enlace(uint8_t tipo, const uint8_t *d, uint16_t n, int idx);
static void procesar(uint8_t *b, uint8_t len, float rssi, float snr);
static void orden(uint8_t tipo, uint8_t *d, uint16_t n, int idx);
static void manda_estado();
static void renombra_bt();           // definida mas abajo, junto al resto del BT
static Preferences prefs;            // el indicativo TIENE que sobrevivir al reinicio
static void guarda_ajustes();
static void despierta_pantalla();
static void apunta_conocido(const uint8_t *bda);
static void olvida_conocidos();
static bool aplica_radio();
static volatile uint32_t n_irq = 0;
static int st_rx = 99;          // ultimo codigo de startReceive(); 0 = bien

/* Armar la recepcion SIEMPRE por aqui.
   El `standby()` previo no es adorno: sin el, la radio recien arrancada no
   levantaba DIO0 en RxDone nunca (en TxDone si), y solo empezaba a recibir
   despues de haber hecho un `radio.receive()` bloqueante — que internamente
   hace justo este standby. */
static int armar_rx()
{
    return radio.startReceive();
}
ICACHE_RAM_ATTR void al_recibir() { hay_paquete = true; n_irq++; }

// ------------------------------------------------------------------- KISS --
/* Un socket TCP solo se escribe si esta LISTO para escribirse.
 *
 * `WiFiClient::write()` reintenta hasta 10 veces con un select de 1 s cada uno:
 * un cliente que deja de leer (se va de cobertura sin cerrar, la app se queda
 * colgada) podria bloquear el nodo DIEZ SEGUNDOS. Eso se lleva por delante el
 * watchdog, la radio y a todos los demas usuarios. Asi que se pregunta antes,
 * con un select de plazo cero, y si no cabe se tira la trama: la voz es tiempo
 * real y un lote perdido es infinitamente mejor que un nodo congelado.
 */
static bool cabe_en(WiFiClient &c)
{
    int fd = c.fd();
    if (fd < 0) return false;
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    struct timeval tv = {0, 0};
    return select(fd + 1, nullptr, &w, nullptr, &tv) > 0 && FD_ISSET(fd, &w);
}

/* Una trama KISS hacia un tubo concreto.
 *
 * Se arma entera y se manda de UNA. Antes salia byte a byte, que por el cable
 * daba igual pero por TCP es un despropósito (una llamada al sistema por byte)
 * y sobre todo hacia imposible saber si la trama cabia: por eso los clientes de
 * WiFi no recibian NADA. El culpable de fondo: `WiFiClient` no implementa
 * `availableForWrite()`, hereda la de `Print`, que devuelve 0 siempre — asi que
 * la comprobacion de sitio era siempre falsa y no se escribia ni un byte, sin
 * ningun error por ninguna parte.
 */
/* El mayor es EV_ESTADO. El buffer de esa linea son 768 B y en el peor caso
   TODOS sus bytes necesitarian escape (aunque siendo texto ASCII eso no puede
   pasar de verdad: ni 0xC0 ni 0xDB son imprimibles). Aun asi se deja holgado,
   porque quedarse corto aqui trunca tramas en silencio igual que alla. */
static uint8_t marco[1600];

static void kiss_a(Tubo &t, uint8_t tipo, const uint8_t *d, size_t n)
{
    if (t.clase == TUBO_LIBRE) return;
    size_t k = 0;
    marco[k++] = KISS_FEND;
    marco[k++] = tipo;
    for (size_t i = 0; i < n && k + 3 < sizeof marco; i++) {
        if (d[i] == KISS_FEND)      { marco[k++] = KISS_FESC; marco[k++] = KISS_TFEND; }
        else if (d[i] == KISS_FESC) { marco[k++] = KISS_FESC; marco[k++] = KISS_TFESC; }
        else                          marco[k++] = d[i];
    }
    marco[k++] = KISS_FEND;

    switch (t.clase) {
    case TUBO_USB: {
        /* NUNCA bloquearse escribiendo en el cable, pero tampoco tirar bytes a
           la primera.
           `Serial.write()` se queda esperando cuando el buffer de salida se
           llena. Costo entero el diagnostico de la actualizacion por Bluetooth:
           el nodo dejaba de confirmar trozos, sin reiniciarse ni dar error,
           simplemente atascado escribiendo en un cable que nadie escuchaba.
           Pero tirar en cuanto no cabe tampoco vale: el buffer son unos cientos
           de bytes y la linea de estado ya es mas larga que eso, asi que
           llegaba PARTIDA, con letras sueltas donde deberia haber campos — y
           con toda la pinta de ser un fallo del snprintf, que no lo era.
           El termino medio: esperar un poco a que el UART drene (a 115200 son
           ~87 us por byte) y solo entonces rendirse. */
        for (size_t i = 0; i < k; i++) {
            uint32_t t0 = millis();
            while (Serial.availableForWrite() <= 8 && millis() - t0 < 15) { }
            if (Serial.availableForWrite() > 8) Serial.write(marco[i]);
        }
        break;
    }
    case TUBO_BT:
        /* Antes aqui habia una guarda de memoria libre, porque la pila SPP de
           Espressif se suicidaba escribiendo sin heap. NimBLE no hace eso: si
           no puede, dice que no. Ver `ble_escribe`. */
        if (bt_conectado) ble_escribe(marco, k);
        break;
    case TUBO_TCP:
    case TUBO_ENLACE:
        if (t.cli.connected() && cabe_en(t.cli)) t.cli.write(marco, k);
        break;
    }
}

/* A todos los anfitriones, menos al que se diga (-1 = a todos).
   El "menos uno" es justo lo que hace falta para devolver la voz del que habla
   a los demas clientes del nodo sin devolversela a el mismo. El enlace queda
   fuera a proposito: por ahi no van eventos del anfitrion, van tramas de aire
   (ver reparte_al_enlace). */
static void kiss_salvo(int salvo, uint8_t tipo, const uint8_t *d, size_t n)
{
    for (int i = 0; i < T_ENL0; i++)
        if (i != salvo && tubos[i].clase != TUBO_LIBRE)
            kiss_a(tubos[i], tipo, d, n);
}

static void kiss_manda(uint8_t tipo, const uint8_t *d, size_t n)
{
    kiss_salvo(-1, tipo, d, n);
}

static void log_txt(const char *s)
{
    kiss_manda(EV_LOG, (const uint8_t *)s, strlen(s));
}

/* Igual, pero SOLO por el cable. El codigo de acceso no puede salir por el
   mismo Bluetooth (ni por el mismo WiFi) que esta autorizando — seria pasar la
   llave por debajo de la puerta. Por USB si: quien tiene el cable ya tiene el
   nodo en la mano, y sin esto no habria forma de probar la autorizacion sin
   mirar la pantalla. */
static void log_usb(const char *s)
{
    kiss_a(tubos[T_USB], EV_LOG, (const uint8_t *)s, strlen(s));
}

// ----------------------------------------------------------------- dedupe --
static bool ya_visto(uint32_t src, uint8_t stream, uint8_t seq, uint8_t tipo)
{
    uint32_t ahora = millis();
    for (int i = 0; i < DEDUPE_N; i++) {
        Vista &v = dedupe[i];
        if (v.src == src && v.stream == stream && v.seq == seq && v.tipo == tipo
            && (ahora - v.t) < DEDUPE_MS)
            return true;
    }
    return false;
}

static void apuntar(uint32_t src, uint8_t stream, uint8_t seq, uint8_t tipo)
{
    Vista &v = dedupe[dedupe_i];
    v.src = src; v.stream = stream; v.seq = seq; v.tipo = tipo; v.t = millis();
    dedupe_i = (dedupe_i + 1) % DEDUPE_N;
}

// ---------------------------------------------------------------- vecinos --
/* `celda`: 1 = es una celda, 0 = no lo es, -1 = no se sabe (no era una baliza).
   El papel solo viaja en la baliza, y las balizas son cada diez minutos; con
   cualquier otra trama se refresca la hora pero NO se toca el papel, o un nodo
   dejaria de ser celda entre baliza y baliza. */
static void apunta_vecino(uint32_t src, int celda = -1)
{
    uint32_t ahora = millis();
    int libre = -1, viejo = 0;
    for (int i = 0; i < VECINOS_N; i++) {
        if (vecinos[i].src == src) {
            vecinos[i].t = ahora;
            if (celda >= 0) vecinos[i].celda = (celda != 0);
            return;
        }
        if (vecinos[i].src == 0 && libre < 0) libre = i;
        if (vecinos[i].t < vecinos[viejo].t) viejo = i;
    }
    int i = (libre >= 0) ? libre : viejo;      // si no cabe, cae el mas antiguo
    vecinos[i].src = src;
    vecinos[i].t = ahora;
    vecinos[i].celda = (celda > 0);
}

/* ¿Hay una CELDA cubriendo a este nodo?
 *
 * Es la pregunta que decide si este nodo repite o se calla, y por tanto la que
 * decide si la red va al 49% o al 100%. Se contesta con las balizas: una celda
 * baliza cada minuto (mas hasta 15 s al azar) diciendo que lo es, y `CELDA_MS`
 * son 150 s, o sea dos balizas y media de margen para no dar por muerta a una
 * celda por haber perdido una. */
static bool hay_celda_a_la_vista()
{
    uint32_t ahora = millis();
    for (int i = 0; i < VECINOS_N; i++)
        if (vecinos[i].src && vecinos[i].celda && vecinos[i].src != mi_src
            && (ahora - vecinos[i].t) < CELDA_MS)
            return true;
    return false;
}

static uint8_t cuenta_vecinos(uint32_t salvo)
{
    uint32_t ahora = millis();
    uint8_t n = 0;
    for (int i = 0; i < VECINOS_N; i++)
        if (vecinos[i].src && vecinos[i].src != salvo && vecinos[i].src != mi_src
            && (ahora - vecinos[i].t) < VECINO_MS)
            n++;
    return n;
}

/* Repetir cuesta caro: MIENTRAS SE REPITE, EL NODO ESTA SORDO, y con lotes de
   voz cada 480 ms eso se come el siguiente. Medido: con dos nodos y saltos=3 se
   pierde el 31% de los lotes; con saltos=1, cero.
   Asi que solo se repite si hay alguien a quien repetir: alguna estacion oida
   ultimamente que no sea ni el emisor ni uno mismo. Con dos nodos se apaga solo
   y sin configurar nada; en cuanto aparece un tercero, vuelve a repetir.
   El perfil manda sobre esto: en PERFIL_FIJO (nodo de cerro) se repite siempre,
   porque alli puede haber estaciones que nos oigan y nosotros a ellas no. */
/* ¿Repito, o me callo?
 *
 * ⚠️ ESTA FUNCION ES EL DISEÑO ENTERO EN CINCO LINEAS. Que un nodo repita o no
 * es lo que separa una red que entrega el 49% de la voz de una que entrega el
 * 100%, y se midio: inundar (todos repiten) da 49%, con celda da 100%.
 *
 * La regla es la de TETRA, TMO/DMO:
 *  - **CELDA** (`PERFIL_FIJO`): es infraestructura, repite SIEMPRE. Para eso
 *    esta puesta ahi.
 *  - **SOLO MI RADIO** (`PERFIL_SOLO`): nunca repite. Lo elige el usuario.
 *  - **AUTO**, que es el caso normal y el que cambia en la v1.27:
 *      · Si hay una CELDA a la vista -> **se calla**. Repetir lo que la celda
 *        ya va a repetir no añade cobertura y si añade ruido: son 128 ms de
 *        canal por cada nodo y por cada lote, y encima destruyen la propia
 *        emision que estaban repitiendo (medido: con 15 nodos densos la entrega
 *        cae al 33%).
 *      · Si NO hay celda -> repite como siempre. Es el modo directo: dos
 *        personas en el campo, sin infraestructura, siguen teniendo malla.
 *        **Esto es innegociable**: el sistema tiene que funcionar encendiendolo
 *        y ya, sin depender de que alguien haya montado nada.
 *
 * O sea que el nodo elige su papel solo, sin configurar nada, y se adapta si la
 * celda se cae. */
static bool hay_a_quien_repetir(uint32_t src)
{
    switch (perfil) {
    case PERFIL_SOLO: return false;
    case PERFIL_FIJO: return true;
    default:
        if (hay_celda_a_la_vista()) return false;
        return cuenta_vecinos(src) > 0;
    }
}

// ------------------------------------------------------------------ radio --
static void cabecera(uint8_t *b, uint8_t tipo, uint8_t saltos,
                     uint32_t src, uint8_t stream, uint8_t seq)
{
    b[0] = PROTO_MAGIC;
    b[1] = mi_canal;
    b[2] = (tipo << 4) | (saltos & 0x0F);
    b[3] = (src >> 16) & 0xFF;
    b[4] = (src >> 8) & 0xFF;
    b[5] = src & 0xFF;
    b[6] = stream;
    b[7] = seq;
}

/* Manda de verdad. Escucha antes: el CAD del SX1278 detecta una portadora LoRa
   aunque este muy por debajo del ruido, que es justo el caso en que un RSSI
   normal no serviria de nada. */
static bool emitir(const uint8_t *b, uint8_t len)
{
    for (int i = 0; i < CAD_INTENTOS; i++) {
        int r = radio.scanChannel();
        if (r == RADIOLIB_CHANNEL_FREE) break;
        delay(20 + random(40));
        if (i == CAD_INTENTOS - 1) return false;      // canal tomado: se descarta
    }
    digitalWrite(P_LED, HIGH);
    /* La linea del amplificador se levanta ANTES y se baja DESPUES: si se
       bajara justo al acabar, el amplificador cortaria el final del paquete. */
    if (pin_ptt) { digitalWrite(pin_ptt, HIGH); delay(ptt_previo); }
    int st = radio.transmit((uint8_t *)b, len);
    if (pin_ptt) { delay(ptt_cola); digitalWrite(pin_ptt, LOW); }
    digitalWrite(P_LED, LOW);
    st_rx = armar_rx();
    if (st == RADIOLIB_ERR_NONE) { n_tx++; return true; }
    return false;
}

static void encolar(const uint8_t *b, uint8_t len, uint32_t cuando)
{
    if (cola_n >= COLA_N) return;                     // mejor perder que retrasar
    Pendiente &p = cola[cola_n++];
    memcpy(p.buf, b, len);
    p.len = len;
    p.cuando = cuando;
}

static void purgar_cola()
{
    uint32_t ahora = millis();
    for (uint8_t i = 0; i < cola_n; i++) {
        if ((int32_t)(ahora - cola[i].cuando) < 0) continue;
        emitir(cola[i].buf, cola[i].len);
        for (uint8_t j = i; j + 1 < cola_n; j++) cola[j] = cola[j + 1];
        cola_n--;
        return;                                       // una por vuelta: no acaparar
    }
}

// --------------------------------------------------------------- recepcion --
/* Pasa una trama del aire a los anfitriones conectados.
 *
 * SIEMPRE, sin mirar si creemos que hay alguien escuchando. Antes esto iba
 * condicionado a `hay_anfitrion`, que solo se enciende cuando el anfitrion
 * MANDA algo: un nodo que solo escucha (justo el caso de un movil que no ha
 * pulsado el PTT todavia) no recibia nada.
 *
 * `salvo` es el tubo al que NO hay que mandarsela. Sirve para el eco local: lo
 * que dice un cliente del nodo tiene que llegar a los OTROS clientes, porque
 * por radio no se van a oir — el nodo no escucha sus propias emisiones. Con
 * salvo = -1 va a todos, que es el caso de lo que llega del aire.
 */
static void entrega_al_anfitrion(const uint8_t *b, uint8_t len, float rssi,
                                 float snr, int salvo)
{
    uint8_t tipo   = b[2] >> 4;
    uint8_t stream = b[6], seq = b[7];
    uint8_t out[8 + MAX_PAYLOAD];
    uint8_t n = 0;
    out[n++] = (uint8_t)(int8_t)constrain((int)rssi, -128, 127);
    out[n++] = (uint8_t)(int8_t)constrain((int)snr, -128, 127);
    out[n++] = b[3]; out[n++] = b[4]; out[n++] = b[5];
    out[n++] = stream;
    uint8_t cuerpo = len - CAB_LEN;
    switch (tipo) {
    case T_INICIO:
        memcpy(out + n, b + CAB_LEN, cuerpo); n += cuerpo;
        kiss_salvo(salvo, EV_INICIO, out, n);
        break;
    case T_VOZ:
        out[n++] = seq;
        memcpy(out + n, b + CAB_LEN, cuerpo); n += cuerpo;
        kiss_salvo(salvo, EV_VOZ, out, n);
        break;
    case T_FIN:
        kiss_salvo(salvo, EV_FIN, out + 2, 4);        // src + stream
        break;
    case T_HOLA:
        memcpy(out + n, b + CAB_LEN, cuerpo); n += cuerpo;
        kiss_salvo(salvo, EV_HOLA, out, n);
        break;
    }
}

/* ------------------------------------------------------- PTT y multiusuario --
 *
 * Con quien se emite. Cada cliente puede declarar SU indicativo (CMD_IDENT); el
 * que no lo haga emite con el del nodo, que es como funcionaba hasta ahora.
 * Esto no es cosmetico: en banda de aficionado cada estacion se identifica con
 * el suyo, y un nodo que da servicio a un grupo estaria emitiendo lo de cinco
 * personas bajo un solo indicativo.
 */
static const char *indicativo_de(int idx)
{
    if (idx >= 0 && tubos[idx].indicativo[0]) return tubos[idx].indicativo;
    return mi_indicativo;
}

static uint32_t src_de(int idx)
{
    if (idx >= 0 && tubos[idx].indicativo[0]) return tubos[idx].src;
    return mi_src;
}

/* Dice a todos quien tiene el microfono. Al que lo tiene se le manda PTT_TUYO y
   a los demas PTT_DE_OTRO con el indicativo, para que la app pueda enseñar
   "habla EA3XYZ" en vez de un PTT que no responde y no se sabe por que. */
static void avisa_ptt()
{
    uint8_t m[1 + MAX_INDICATIVO];
    const char *q = ptt_de >= 0 ? indicativo_de(ptt_de) : "";
    uint8_t li = strlen(q);
    if (li > MAX_INDICATIVO) li = MAX_INDICATIVO;
    for (int i = 0; i < T_ENL0; i++) {
        if (tubos[i].clase == TUBO_LIBRE) continue;
        if (ptt_de < 0)      m[0] = PTT_LIBRE;
        else if (i == ptt_de) m[0] = PTT_TUYO;
        else                  m[0] = PTT_DE_OTRO;
        memcpy(m + 1, q, li);
        kiss_a(tubos[i], EV_PTT, m, 1 + li);
    }
}

static bool coge_ptt(int idx)
{
    if (ptt_de == idx) return true;
    if (ptt_de >= 0) {
        // Ocupado: se le dice a quien pregunta, y no se le corta al que habla.
        uint8_t m[1 + MAX_INDICATIVO];
        const char *q = indicativo_de(ptt_de);
        uint8_t li = strlen(q);
        m[0] = PTT_DE_OTRO;
        memcpy(m + 1, q, li);
        kiss_a(tubos[idx], EV_PTT, m, 1 + li);
        return false;
    }
    ptt_de = idx;
    t_ptt = millis();
    avisa_ptt();
    return true;
}

static void suelta_ptt(const char *motivo)
{
    if (ptt_de < 0) return;
    /* Si se solto por las malas (TOT, o el cliente desaparecio) hay que cerrar
       el stream en el aire igualmente, o los receptores se quedan esperando un
       FIN que no llega y con el audio a medias. */
    if (transmitiendo) {
        uint8_t b[CAB_LEN];
        cabecera(b, T_FIN, saltos_def, src_de(ptt_de), tx_stream, ++tx_seq);
        apuntar(src_de(ptt_de), tx_stream, tx_seq, T_FIN);
        emitir(b, CAB_LEN);
        transmitiendo = false;
    }
    ptt_de = -1;
    redibujar = true;
    avisa_ptt();
    if (motivo) log_txt(motivo);
}

/* Vigila al que tiene el microfono. Dos relojes distintos a proposito:
   - TOT (3 min): el mismo limite que la app PTT. Un PTT enganchado en un
     bolsillo bloquea el canal para todo el mundo.
   - mudo (5 s): el cliente sigue conectado pero dejo de mandar voz. Pasa al
     perderse la cobertura WiFi sin que el socket se entere. */
static void vigila_ptt()
{
    if (ptt_de < 0) return;
    uint32_t ahora = millis();
    if (ahora - t_ptt > PTT_MAX_MS) { suelta_ptt("TOT: 3 minutos hablando"); return; }
    if (ahora - tubos[ptt_de].t_ultimo > PTT_MUDO_MS)
        suelta_ptt("el que hablaba dejo de mandar");
}

/* ------------------------------------------------------------------ ENLACE --
 *
 * Une por Internet dos zonas de cobertura LoRa que no se oyen entre si. Por el
 * enlace viajan TRAMAS DEL AIRE tal cual, no ordenes: asi el nodo del otro lado
 * las mete por `procesar()` como si las hubiera oido por la radio y hereda
 * gratis el dedupe, la entrega a sus anfitriones y la repeticion.
 *
 * Los bucles los corta el dedupe que ya existe: si A manda a B y B lo repite y
 * se lo devuelve, A ya tiene esa (src, stream, seq, tipo) apuntada.
 */
/* A TODOS los nodos enlazados menos al que la trajo. Es el mismo patron que el
   eco local entre clientes, y es lo que convierte una pareja de nodos en una
   red: un nodo intermedio reparte a los demas lo que le llega de uno.
   Los bucles no hay que pararlos aqui: los corta el dedupe de siempre, porque
   lo que vuelve rebotado ya esta apuntado con su (src, stream, seq, tipo). */
static void reparte_al_enlace(const uint8_t *b, uint8_t len, int salvo)
{
    for (int i = T_ENL0; i < TUBOS_N; i++)
        if (i != salvo && tubos[i].clase == TUBO_ENLACE)
            kiss_a(tubos[i], CMD_AIRE, b, len);
}

static void del_enlace(uint8_t tipo, const uint8_t *d, uint16_t n, int idx)
{
    if (tipo != CMD_AIRE) return;
    if (n < CAB_LEN || n > CAB_LEN + MAX_PAYLOAD) return;
    uint8_t copia[CAB_LEN + MAX_PAYLOAD];
    memcpy(copia, d, n);
    /* Se pasa a los OTROS enlaces antes de procesarla: asi un nodo intermedio
       encadena ubicaciones. `procesar` no lo hara (marca rssi=127 justo para no
       devolver al enlace lo que vino de un enlace). */
    reparte_al_enlace(copia, (uint8_t)n, idx);
    /* rssi/snr marcados como "no vienen del aire": 127 es imposible por radio y
       la app lo puede pintar distinto (llego por Internet, no por LoRa). */
    procesar(copia, (uint8_t)n, 127, 0);
}

static void procesar(uint8_t *b, uint8_t len, float rssi, float snr)
{
    if (len < CAB_LEN || b[0] != PROTO_MAGIC) { n_malas++; return; }
    if (b[1] != mi_canal) return;                     // otro canal: ni repetir

    uint8_t tipo   = b[2] >> 4;
    uint8_t saltos = b[2] & 0x0F;
    uint32_t src   = ((uint32_t)b[3] << 16) | ((uint32_t)b[4] << 8) | b[5];
    uint8_t stream = b[6], seq = b[7];

    if (ya_visto(src, stream, seq, tipo)) { n_dup++; return; }
    apuntar(src, stream, seq, tipo);
    n_rx++;
    t_ultimo_rx = millis();
    ultimo_rssi = (int)rssi;
    redibujar = true;
    despierta_pantalla();
    // El indicativo solo viene en INICIO y HOLA; en la voz hay que recordar el
    // que dio esa estacion al abrir, que es justo para lo que sirven.
    if (tipo == T_INICIO || tipo == T_HOLA) {
        uint8_t salto = (tipo == T_INICIO) ? 1 : 2;     // modo / flags+bateria
        /* En la baliza, detras del indicativo puede venir `\0` + el nombre del
           nodo (v1.30+). Se corta en el cero: aqui solo interesa el indicativo,
           y el nombre viaja entero hasta el anfitrion (ver `entrega_al_anfitrion`),
           que es quien lo tiene que enseñar. */
        /* El papel del vecino solo viaja aqui: en la baliza, primer byte tras
           la cabecera. Con eso un cliente sabe si tiene celda encima. */
        if (tipo == T_HOLA && len > CAB_LEN)
            apunta_vecino(src, (b[CAB_LEN] & HOLA_CELDA) ? 1 : 0);
        int li = (int)len - CAB_LEN - salto;
        if (li > 0) {
            if (li > MAX_INDICATIVO) li = MAX_INDICATIVO;
            const uint8_t *ind = b + CAB_LEN + salto;
            for (int k = 0; k < li; k++) if (ind[k] == 0) { li = k; break; }
            memcpy(ultimo_ind, ind, li);
            ultimo_ind[li] = 0;
        }
    }

    // 1) entregar al anfitrion (a todos los que haya).
    entrega_al_anfitrion(b, len, rssi, snr, -1);

    /* 1b) y al nodo del otro lado del enlace, si lo hay. Salvo que venga
       precisamente de ahi (rssi 127 = no salio de la radio): devolverselo seria
       tirar de la linea para nada, aunque el dedupe del otro lado lo pare. */
    if (rssi < 127) reparte_al_enlace(b, len, -1);

    apunta_vecino(src);

    // 2) repetir, si queda salto, no es nuestro y hay a quien repetir
    if (saltos > 0 && src != mi_src && !hay_a_quien_repetir(src)) {
        n_calladas++;
    } else if (saltos > 0 && src != mi_src) {
        uint8_t copia[CAB_LEN + MAX_PAYLOAD];
        memcpy(copia, b, len);
        copia[2] = (tipo << 4) | (saltos - 1);
        // La espera es ALEATORIA a proposito: si todos los repetidores del
        // alcance repiten en el mismo instante, se destruyen entre ellos.
        encolar(copia, len, millis() + JITTER_MIN_MS +
                random(JITTER_MAX_MS - JITTER_MIN_MS));
        n_repetidas++;
    }
}

// --------------------------------------------------------------- pantalla --
//
// Para el papel de TESTIGO (una placa suelta repitiendo, sin movil) la pantalla
// es lo unico que dice si el nodo esta vivo. Se dibuja tambien cuando no hay
// anfitrion, a proposito.
static uint8_t bateria_pct();

/* Cualquier cosa que merezca mirarse enciende la pantalla: trafico, PTT, un
   cambio de estado del canal o el boton. Lo demas la deja dormirse sola. */
static void despierta_pantalla()
{
    t_actividad = millis();
    if (!pantalla_on) { pantalla_on = true; redibujar = true; }
}

static void gestiona_pantalla()
{
    if (!hay_oled) return;
    bool debe = (pantalla_modo == PANTALLA_FIJA) ||
                (pantalla_modo == PANTALLA_AUTO &&
                 millis() - t_actividad < PANTALLA_MS);
    if (pantalla_modo == PANTALLA_OFF) debe = false;
    if (debe == pantalla_on) return;
    pantalla_on = debe;
    // El SSD1306 tiene una orden para apagar el panel sin perder la imagen ni
    // reinicializarlo: consume practicamente cero y despierta al instante.
    oled.ssd1306_command(pantalla_on ? SSD1306_DISPLAYON : SSD1306_DISPLAYOFF);
    if (pantalla_on) redibujar = true;
}

static void pinta()
{
    if (!hay_oled || !pantalla_on) return;
    oled.clearDisplay();

    // Mientras haya un emparejamiento en curso, la pantalla no enseña otra
    // cosa: es el unico momento en que de verdad hay que mirarla.
    if (ota_curso) {
        oled.setTextSize(1);
        oled.setTextColor(SSD1306_WHITE);
        oled.setCursor(0, 0);
        oled.print("ACTUALIZANDO");
        oled.setTextSize(2);
        oled.setCursor(20, 20);
        oled.printf("%3u%%", (unsigned)(ota_total ? ota_bytes * 100ULL / ota_total : 0));
        oled.setTextSize(1);
        oled.setCursor(0, 50);
        oled.print("No apagues el nodo");
        oled.display();
        redibujar = false;
        t_pantalla = millis();
        return;
    }

    /* Hay alguien esperando a que le dejen entrar: el codigo lo llena todo,
       porque es el unico momento en que de verdad hay que mirar la pantalla.
       Vale igual para el movil por Bluetooth y para un cliente por WiFi. */
    bool esperando = (bt_conectado && !bt_autorizado);
    for (int i = T_TCP0; !esperando && i < T_ENL0; i++)
        if (tubos[i].clase == TUBO_TCP && !tubos[i].autorizado) esperando = true;
    if (esperando) {
        oled.setTextSize(1);
        oled.setTextColor(SSD1306_WHITE);
        oled.setCursor(0, 0);
        oled.print("CODIGO DE ACCESO");
        oled.setTextSize(3);
        oled.setCursor(8, 20);
        oled.printf("%06u", (unsigned)bt_codigo);
        oled.setTextSize(1);
        oled.setCursor(0, 52);
        oled.print("Escribelo en la app");
        oled.display();
        redibujar = false;
        t_pantalla = millis();
        return;
    }

    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    /* EL NOMBRE DEL CACHARRO A LA IZQUIERDA, EL INDICATIVO A LA DERECHA.
     *
     * Antes aqui ponia "PTT LoRa" fijo y el indicativo. Y eso deja la pantalla
     * INSERVIBLE para lo unico que solo ella puede hacer: decirte **cual de las
     * cajas que tienes delante es esta**. Los nodos de un mismo operador llevan
     * todos SU indicativo (es lo legalmente correcto y lo que pone la app sola),
     * asi que tres placas en la mesa enseñaban las tres `C31AG` y no habia forma
     * de saber cual subir al tejado. Paso de verdad el 8-sep-2026.
     * El indicativo identifica la ESTACION y su sitio es el aire; el NOMBRE
     * identifica el CACHARRO y su sitio es esta pantalla. */
    oled.setCursor(0, 0);
    {
        /* 21 caracteres de ancho (128 px / 6 px). Se le reserva su sitio al
           indicativo y el nombre se recorta con lo que quede: mejor un nombre a
           medias que dos textos pisandose. */
        int hueco = 21 - (int)strlen(mi_indicativo) - 1;
        if (hueco < 4) hueco = 4;
        char n[22];
        snprintf(n, sizeof n, "%.*s", hueco, nombre_nodo[0] ? nombre_nodo : "nodo");
        oled.print(n);
    }
    oled.setCursor(128 - 6 * strlen(mi_indicativo), 0);
    oled.print(mi_indicativo);
    oled.drawFastHLine(0, 10, 128, SSD1306_WHITE);

    oled.setTextSize(2);
    oled.setCursor(0, 14);
    if (transmitiendo)       oled.print("TX");
    else if (canal_ocupado)  oled.print("RX");
    else                     oled.print("--");
    oled.setTextSize(1);
    oled.setCursor(34, 14);
    oled.print(transmitiendo ? "transmitiendo"
                             : (canal_ocupado ? "ocupado" : "libre"));
    oled.setCursor(34, 24);
    {
        uint8_t cl = cuenta_clientes();
        if (cl) { oled.print(cl); oled.print(" x wifi"); }
        else oled.print(bt_conectado ? "movil BLE" :
                        (hay_anfitrion ? "puente USB" : (bt_activo ? "nodo" : "BT off")));
    }
    oled.setCursor(0, 24);
    oled.print(perfil == PERFIL_FIJO ? "REPETIDOR" :
               (perfil == PERFIL_SOLO ? "solo yo" : "auto"));
    if (perfil == PERFIL_AUTO && cuenta_vecinos(0) == 0) {
        oled.setCursor(96, 24);
        oled.print("solo");            // nadie a la vista: no repite
    }

    oled.setCursor(0, 34);
    if (ptt_de >= 0) {
        // Con varios usuarios importa mas quien tiene el microfono AHORA que
        // quien se oyo la ultima vez.
        oled.print("habla ");
        oled.print(indicativo_de(ptt_de));
    } else if (ultimo_ind[0]) {
        oled.print(ultimo_ind);
        oled.print(" ");
        oled.print(ultimo_rssi);
        oled.print("dBm");
    } else {
        oled.print("sin trafico aun");
    }

    oled.setCursor(0, 46);
    oled.printf("%.3f sf%u %udBm", frecuencia, (unsigned)sf, potencia);
    if (wifi_activo) {
        oled.setCursor(110, 24);
        oled.print(red_modo == RED_AP ? "AP" : "wf");
    }
    /* LA SEÑAL WiFi, EN LA PANTALLA.
     *
     * En un nodo instalado en alto la pregunta no es si el WiFi "va", es si va
     * con margen: a -80 dBm engancha en la mesa y se cae en cuanto llueve. Y en
     * una placa sin Bluetooth **la pantalla es el unico instrumento que queda**
     * — no hay app a la que preguntarle desde alli arriba. Asi que el dato se
     * pone donde se puede leer subiendo una vez, con la placa en la mano. */
    if (wifi_activo && red_modo == RED_CLIENTE) {
        oled.setCursor(0, 56);
        if (WiFi.status() == WL_CONNECTED) {
            long q = WiFi.RSSI();
            oled.printf("%s %lddBm", (wifi_cual ? "red2" : "red1"), q);
            // Una palabra de veredicto: un numero suelto no dice si vale.
            oled.setCursor(96, 56);
            oled.print(q > -67 ? "bien" : (q > -75 ? "justo" : "MAL"));
        } else {
            oled.print(wifi_ssid2.length()
                       ? (wifi_cual ? "buscando red2" : "buscando red1")
                       : "sin wifi");
        }
    }
    {
        uint8_t enl = 0;
        for (int i = T_ENL0; i < TUBOS_N; i++)
            if (tubos[i].clase == TUBO_ENLACE) enl++;
        if (enl) {
            oled.setCursor(104, 34);
            oled.print("E");
            oled.print(enl);              // enlaces en pie
        }
    }
    oled.setCursor(0, 56);
    uint8_t vec = cuenta_vecinos(0);
    oled.printf("rx%lu tx%lu rp%lu v%u %u%%",
                n_rx, n_tx, n_repetidas, vec, bateria_pct());

    oled.display();
    redibujar = false;
    t_pantalla = millis();
}

// ------------------------------------------------------------- anfitrion ---
static uint8_t bateria_pct()
{
#ifdef PLACA_TBEAM
    // En la T-Beam la bateria cuelga del AXP2101 y GPIO35 es del PMU, no del
    // divisor: leerlo con analogRead da basura.
    if (hay_pmu) {
        int p = pmu.getBatteryPercent();
        return (p < 0) ? 0 : (uint8_t)p;
    }
    return 0;
#else
    // Divisor 2:1 en GPIO35. 4,2 V = 100%, 3,3 V = 0%. Aproximado y de sobra.
    uint32_t mv = (uint32_t)(analogRead(P_BAT) * 2 * 3300.0 / 4095.0);
    if (mv <= 3300) return 0;
    if (mv >= 4200) return 100;
    return (mv - 3300) * 100 / 900;
#endif
}

#ifdef PLACA_TBEAM
/* -------------------------------------------------------- GPS de la T-Beam --
 *
 * La placa lo lleva puesto desde el primer dia y no se usaba. Con el, una
 * T-Beam sabe donde esta SIN MOVIL: eso es lo que da posicion a un nodo
 * plantado en un cerro, y de paso a la celda cuando la celda sea una de estas.
 *
 * SIN LIBRERIA, y no por ahorrar: TinyGPS++ trae el parser entero de NMEA
 * —fecha, hora, satelites, HDOP, rumbo, curso— y de todo eso aqui hace falta
 * una linea. Son cuarenta lineas propias frente a una dependencia mas en un
 * firmware que se actualiza por radio y donde cada kB cuenta para la ranura de
 * OTA.
 *
 * Se lee $..RMC y no $..GGA porque el RMC trae el AVISO DE VALIDEZ (`A` contra
 * `V`) en un campo propio: un GPS sin fijar sigue mandando tramas, con ceros o
 * con la ultima posicion conocida, y sin mirar ese campo se publica basura.
 */
static char gps_linea[100];
static uint8_t gps_n = 0;
static uint32_t gps_ultimo = 0;      // ultima trama VALIDA (0 = ninguna aun)
static bool gps_visto = false;       // ha llegado algo por la UART

/* Un campo de NMEA: devuelve el puntero al campo `i` (0 = la cabecera) o NULL.
   Trabaja sobre la linea entera sin copiarla ni trocearla. */
static const char *nmea_campo(const char *l, int i)
{
    for (; i > 0; i--) {
        l = strchr(l, ',');
        if (!l) return NULL;
        l++;
    }
    return l;
}

/* "4231.5678" + 'N' -> diezmillonesimas de grado. El formato de NMEA son
   GRADOS Y MINUTOS PEGADOS (ddmm.mmmm), que es la trampa clasica: leerlo como
   grados decimales da una posicion plausible y equivocada por decenas de km. */
static bool nmea_grados(const char *c, char hemi, int32_t *out)
{
    const char *punto = strchr(c, '.');
    if (!punto || punto - c < 3) return false;
    int ngrados = (int)(punto - c) - 2;          // 2 para latitud, 3 para longitud
    char g[4] = {0};
    if (ngrados > 3) return false;
    memcpy(g, c, ngrados);
    double grados = atof(g);
    double minutos = atof(c + ngrados);
    double v = grados + minutos / 60.0;
    if (hemi == 'S' || hemi == 'W') v = -v;
    *out = (int32_t)llround(v * 1e7);
    return true;
}

static void gps_linea_lista()
{
    /* $GPRMC, $GNRMC, $GLRMC... el prefijo de constelacion cambia segun lo que
       vea el receptor, asi que se mira el TIPO, no las cinco letras. */
    if (strncmp(gps_linea, "$G", 2) || strncmp(gps_linea + 3, "RMC", 3)) return;
    const char *estado = nmea_campo(gps_linea, 2);
    if (!estado || *estado != 'A') return;                 // sin fijar: se ignora
    const char *la = nmea_campo(gps_linea, 3), *nl = nmea_campo(gps_linea, 4);
    const char *lo = nmea_campo(gps_linea, 5), *el = nmea_campo(gps_linea, 6);
    if (!la || !nl || !lo || !el) return;
    int32_t vla, vlo;
    if (!nmea_grados(la, *nl, &vla)) return;
    if (!nmea_grados(lo, *el, &vlo)) return;
    bool primera = !hay_pos;
    /* El GPS propio NO pisa una posicion FIJA. Si a alguien le da por poner una
       T-Beam de celda y teclearle la posicion, es que quiere esa y no la que
       diga el GPS: una posicion tecleada es una decision, y el GPS deriva unos
       metros cada vez que arranca. Al movil SI lo pisa: el dato del propio
       aparato manda sobre el prestado. */
    if (pos_origen == POS_FIJA && hay_pos) return;
    pos_lat = vla; pos_lon = vlo;
    pos_origen = POS_GPS;
    hay_pos = true;
    t_pos = millis();
    gps_ultimo = t_pos;
    if (primera) {
        char m[64];
        snprintf(m, sizeof m, "GPS fijado: %.6f,%.6f", vla / 1e7, vlo / 1e7);
        log_txt(m);
    }
}

static void gps_atiende()
{
    while (Serial1.available()) {
        char c = (char)Serial1.read();
        gps_visto = true;
        if (c == '\n' || c == '\r') {
            if (gps_n) { gps_linea[gps_n] = 0; gps_linea_lista(); }
            gps_n = 0;
        } else if (gps_n < sizeof gps_linea - 1) {
            gps_linea[gps_n++] = c;
        } else {
            gps_n = 0;                 // linea imposible: se tira entera
        }
    }
}
#endif

static void manda_hola()
{
    /* Cabecera + flags + bateria + indicativo + \0 + nombre + \0 + lat + lon. */
    uint8_t b[CAB_LEN + 2 + MAX_INDICATIVO + 1 + sizeof nombre_nodo + 1 + 8];
    /* EN LA BALIZA VA LO QUE EL CACHARRO SABE DE SI MISMO: la posicion tecleada
       de una celda y la de su propio GPS. Las dos describen un aparato, y un
       aparato tiene que estar en el mapa aunque no hable nadie en todo el dia —
       un tracker de LoRa APRS hace exactamente esto.
       La que le presta un MOVIL no: esa es la de una persona, y sale solo en el
       INICIO de cada transmision. Ver la nota de protocolo.h. */
    const bool con_pos = pos_vale() && pos_publica &&
                         (pos_origen == POS_FIJA || pos_origen == POS_GPS);
    cabecera(b, T_HOLA, saltos_def, mi_src, 0, 0);
    uint8_t n = CAB_LEN;
    /* ⚠️ `HOLA_REPETIDOR` SALE DE SI VA A REPETIR DE VERDAD, no del perfil.
       Estaba puesto con `perfil != PERFIL_SOLO` y **la baliza mentia**: un nodo
       en `auto` con una celda a la vista esta en `modo=cliente` y NO repite,
       pero se anunciaba como `repetidor`. Visto en vivo el 8-sep-2026. Con eso
       el mapa de la red enseña repetidores donde no los hay — y ese mapa es
       justo lo que se usa para decidir donde poner la siguiente celda. */
    b[n++] = (hay_a_quien_repetir(0) ? HOLA_REPETIDOR : 0)
             | ((hay_anfitrion || bt_conectado) ? HOLA_PUENTE : 0)
             | (perfil == PERFIL_FIJO ? HOLA_CELDA : 0)
             | (con_pos ? HOLA_POS : 0);
    b[n++] = bateria_pct();
    uint8_t li = strlen(mi_indicativo);
    memcpy(b + n, mi_indicativo, li); n += li;
    /* Y EL NOMBRE DEL CACHARRO DETRAS, separado por un cero.
     *
     * Hace falta porque **los nodos de un mismo operador llevan todos SU
     * indicativo** (es lo legalmente correcto, y ademas la app se lo pone
     * sola): tres balizas de `C31AG` y no hay forma de saber cual es cual.
     * El indicativo identifica la ESTACION, el nombre identifica el CACHARRO.
     *
     * Separador de cero y no un campo de longitud, a proposito: **asi es
     * compatible hacia atras**. Un firmware anterior a la v1.30 lee "el resto
     * de la trama" como indicativo, se encuentra el cero por el camino y al
     * usarlo como cadena ve exactamente el indicativo, sin enterarse de que
     * detras venia algo mas.
     *
     * Cuesta 10 ms de aire en una trama que sale UNA VEZ CADA DIEZ MINUTOS por
     * nodo: con tres nodos, un 0,005% del canal. */
    /* El cero del nombre sale TAMBIEN CON EL NOMBRE VACIO si detras va la
       posicion: los campos de la cola se cuentan por separadores, y saltarse
       uno correria la posicion al hueco del nombre. Un nodo sin nombre y con
       posicion manda `indicativo \0 \0 lat lon`, que es exactamente lo que el
       que lee espera encontrar. */
    if (nombre_nodo[0] || con_pos) {
        b[n++] = 0;
        uint8_t ln = strlen(nombre_nodo);
        memcpy(b + n, nombre_nodo, ln); n += ln;
    }
    /* Y LA POSICION DETRAS DE OTRO CERO (v1.34). Little-endian a mano y no un
       memcpy del int32: el orden de bytes en la trama es parte del protocolo y
       no debe depender de que el que compile sea un ESP32. */
    if (con_pos) {
        b[n++] = 0;
        uint32_t la = (uint32_t)pos_lat, lo = (uint32_t)pos_lon;
        for (int i = 0; i < 4; i++) b[n++] = (uint8_t)(la >> (8 * i));
        for (int i = 0; i < 4; i++) b[n++] = (uint8_t)(lo >> (8 * i));
    }
    apuntar(mi_src, 0, 0, T_HOLA);          // que no nos lo repitan de vuelta
    emitir(b, n);
    /* Y por los enlaces: asi cada nodo sabe quien hay en TODA la red, no solo a
       quien alcanza su antena. Es lo que permite que la lista de estaciones sea
       de la malla entera. Cuesta muy poco — una baliza cada diez minutos por
       nodo — y el reflector nunca las arbitra, precisamente para que no se
       pierdan. */
    reparte_al_enlace(b, n, -1);
    /* El jitter va aqui, restandolo del proximo vencimiento: asi el intervalo
       real es HOLA_MS + [0, HOLA_JITTER_MS). Ver la nota de HOLA_MS. */
    /* ⛔ EL JITTER VA EN EL INTERVALO, NUNCA SUMADO A LA MARCA DE TIEMPO.
     *
     * Aqui estuvo `t_ultima_hola = millis() + random(HOLA_JITTER_MS)` y fue un
     * fallo serio, en produccion y con los tres nodos: poner la marca en el
     * FUTURO hace que `millis() - t_ultima_hola` **se desborde** (son uint32_t,
     * la resta da ~4.290.000.000) y esa cifra siempre supera el intervalo. O
     * sea que la condicion de balizar se cumple SIEMPRE y **el nodo baliza en
     * bucle** toda la ventana del jitter, frenado solo por el chequeo de canal
     * ocupado.
     * Resultado medido el 8-sep-2026: tormenta de balizas y **30 tramas
     * corruptas** en la celda en un minuto, con balizas basura que hasta se
     * anunciaban como CELDA con nombres ilegibles. Se queria romper la
     * sincronia entre nodos y se monto un bucle.
     * La forma correcta es esta: la marca es SIEMPRE el pasado (ahora) y lo que
     * varia es cuanto se espera. */
    t_ultima_hola = millis();
    hola_espera = HOLA_MS + (uint32_t)random(HOLA_JITTER_MS);
}

/* Por que se reinicio la ultima vez. En un nodo desatendido es la diferencia
   entre "se colgo" y "se quedo sin tension", que se arreglan de formas muy
   distintas. */
static const char *motivo_reset()
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "arranque";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panico";
    case ESP_RST_INT_WDT:  return "wdt-interrupciones";
    case ESP_RST_TASK_WDT: return "wdt-tarea";
    case ESP_RST_WDT:      return "wdt";
    case ESP_RST_BROWNOUT: return "CAIDA-DE-TENSION";
    case ESP_RST_EXT:      return "externo";
    default:               return "?";
    }
}

/* Resumen de la red en una linea, para el estado y para la app: modo, IP,
   cuantos clientes hay colgados, quien tiene el microfono y si el enlace esta
   en pie. En un nodo remoto esto es lo unico que se puede mirar. */
static uint8_t cuenta_clientes()
{
    uint8_t k = 0;
    for (int i = T_TCP0; i < T_ENL0; i++)
        if (tubos[i].clase == TUBO_TCP) k++;
    return k;
}

static String red_texto()
{
    String r;
    if (!wifi_activo)            r = "no";
    else if (red_modo == RED_AP) r = "ap:" + WiFi.softAPIP().toString() +
                                     "/" + String(WiFi.softAPgetStationNum());
    else if (WiFi.status() == WL_CONNECTED) {
        /* Con dos redes guardadas hay que decir POR CUAL ha entrado: si no,
           mirando el estado no hay forma de saber si el nodo esta donde crees
           que esta. */
        r = WiFi.localIP().toString();
        const String &ss = wifi_cual ? wifi_ssid2 : wifi_ssid;
        if (ss.length()) r += "(" + ss + ")";
        r += " señal=" + String((int)WiFi.RSSI()) + "dBm";
    } else {
        r = "buscando";
        if (wifi_ssid2.length())
            r += wifi_cual ? "(segunda)" : "(primera)";
    }
    r += " clientes=" + String(cuenta_clientes());
    r += pedir_codigo ? " codigo=si" : " codigo=no";
    r += " ptt=";
    r += (ptt_de >= 0) ? indicativo_de(ptt_de) : "libre";
    /* Los enlaces, uno a uno: en una red de varias ubicaciones lo que hace
       falta saber es CUALES estan en pie, no cuantos. */
    for (int k = 0; k < SALIENTES_N; k++) {
        if (enlace_host[k].length() == 0) continue;
        bool pie = false;
        for (int i = T_ENL0; i < TUBOS_N; i++)
            if (tubos[i].clase == TUBO_ENLACE && tubos[i].salida_k == k) pie = true;
        r += " enlace=" + enlace_host[k] + ":" + String(enlace_puerto[k]) +
             (pie ? "(en pie)" : "(caido)");
    }
    if (mando_host.length()) {
        bool pie = (mando_tubo >= 0 && tubos[mando_tubo].clase == TUBO_TCP);
        r += " mando=" + mando_host + ":" + String(mando_puerto) +
             (pie ? "(en pie)" : "(caido)");
    }
    uint8_t entrantes = 0;
    for (int i = T_ENL0; i < TUBOS_N; i++)
        if (tubos[i].clase == TUBO_ENLACE && tubos[i].salida_k < 0) entrantes++;
    if (entrantes) r += " entrantes=" + String(entrantes);
    return r;
}

/* La posicion tal y como se lee en el estado. Cuatro casos y todos importan:
   sin posicion, fija, viva y fresca, y viva pero caducada — este ultimo se
   enseña con la edad, porque "hace 40 min" explica por que el nodo ha dejado
   de balizarla. */
static String pos_texto()
{
    if (!hay_pos) return "no";
    char s[64];
    double la = pos_lat / 1e7, lo = pos_lon / 1e7;
    if (pos_origen == POS_FIJA) {
        snprintf(s, sizeof s, "%.6f,%.6f(fija)", la, lo);
    } else {
        unsigned long edad = (millis() - t_pos) / 60000UL;
        snprintf(s, sizeof s, "%.6f,%.6f(%s,%lum%s)", la, lo,
                 pos_origen == POS_GPS ? "gps" : "movil", edad,
                 pos_vale() ? "" : ",CADUCADA");
    }
    if (!pos_publica) strncat(s, "(no se publica)", sizeof s - strlen(s) - 1);
    return String(s);
}

/* Como va el GPS, en la placa que lo tiene. Tres respuestas y las tres son
   diagnostico: no hay GPS, hay GPS pero no fija (antena tapada, arranque en
   frio), o fija. Sin este campo, "el nodo no da posicion" tiene tres causas
   distintas y ninguna forma de distinguirlas desde fuera. */
static String gps_texto()
{
#ifdef PLACA_TBEAM
    if (!gps_visto) return " gps=sin-señal";
    if (!gps_ultimo) return " gps=sin-fijar";
    char s[32];
    snprintf(s, sizeof s, " gps=fijado(%lus)",
             (unsigned long)((millis() - gps_ultimo) / 1000UL));
    return String(s);
#else
    return "";
#endif
}

static void manda_estado()
{
    /* 512, y hubo que subirlo desde 384: entonces la linea llegaba a 392
       caracteres y se cortaba justo en los ultimos campos — `mando=`, `ptt=` y
       los enlaces desaparecian sin ningun aviso, que es la peor forma de perder
       informacion de diagnostico.
       Ahora son 768 y ademas **se comprueba de verdad** (ver el aviso despues
       del snprintf), que es lo que aquella nota pedia y no se hizo.
       ⚠️ Aclaracion, porque me equivoque diagnosticando el 8-sep-2026: cuando
       `btmodo=` no aparecia en el estado NO era por esto — la linea eran 447
       caracteres y cabia de sobra. Era que **la placa arrancaba desde la otra
       ranura OTA** y el firmware que corria no era el que se estaba flasheando.
       Ver la nota de arriba del fichero sobre `boot_app0`. */
    char s[768];
    snprintf(s, sizeof s,
             "v%s %s perfil=%s activo=%lum canal=%u saltos=%u %.3fMHz sf%u bw%.0f cr%u %udBm "
             "hw=%.0f-%.0fMHz/%u-%udBm banda=%.0f-%.0fMHz rx=%lu tx=%lu rep=%lu "
             "dup=%lu mal=%lu call=%lu vec=%u irq=%lu strx=%d rssi=%.0f bat=%u reset=%s "
             "heap=%u/%u nombre=%s bt=%s ok=%d visible=%d mac=%s%s btatasco=%lu btsalta=%lu ble=%u modo=%s btmodo=%u ampli=%u/%u/%u preamb=%u pos=%s%s wifi=%s",
             VERSION, mi_indicativo,
             perfil == PERFIL_FIJO ? "repetidor-fijo" :
             (perfil == PERFIL_SOLO ? "solo-nodo" : "auto"),
             (unsigned long)(millis() / 60000UL),
             mi_canal, saltos_def, frecuencia, (unsigned)sf, ancho, (unsigned)cr,
             potencia, (float)FREQ_MIN, (float)FREQ_MAX,
             (unsigned)POT_MIN, (unsigned)POT_MAX,
             (float)BANDA_MIN, (float)BANDA_MAX,
             n_rx, n_tx, n_repetidas, n_dup, n_malas,
             n_calladas, cuenta_vecinos(0),
             (unsigned long)n_irq, st_rx, radio.getRSSI(false, true), bateria_pct(),
             motivo_reset(),
             /* Memoria libre y el minimo historico. Desde la v1.26 hay MUCHO
                mas margen (Bluetooth Classic se llevaba ~90 kB de heap y 828 kB
                de flash; NimBLE son unos pocos kB), pero el dato se queda: esta
                placa no tiene PSRAM y quedarse sin memoria sigue siendo un
                fallo silencioso. Este par de numeros es lo que permite ver de
                un vistazo cuanto se gano con el cambio. */
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
             nombre_nodo, nombre_bt, (int)bt_ok, bt_visible, bt_mac,
             /* Dos fuentes A PROPOSITO: nuestra bandera y la cuenta que lleva
                NimBLE. Si no coinciden, es que se perdio un `onDisconnect` — y
                sin este dato eso es indistinguible de un movil de verdad
                conectado, que fue exactamente el rato perdido del 8-sep. */
             bt_conectado ? " (movil conectado)" : "",
             (unsigned long)bt_atasco, (unsigned long)bt_saltadas,
             (unsigned)(ble_srv ? ble_srv->getConnectedCount() : 0),
             /* Que se vea de un vistazo si el nodo esta repitiendo o callado, y
                por que. Sin este campo, "no repite" y "no hay nadie" son
                indistinguibles desde fuera. */
             perfil == PERFIL_FIJO ? "celda" :
             (perfil == PERFIL_SOLO ? "solo" :
              (hay_celda_a_la_vista() ? "cliente(hay celda)" : "malla")),
             (unsigned)bt_modo,
             (unsigned)pin_ptt, (unsigned)ptt_previo, (unsigned)ptt_cola,
             (unsigned)preambulo,
             /* En grados de verdad, aunque dentro sean enteros: este campo lo
                lee una persona y lo pega en un mapa. Y dice de donde sale, que
                es la mitad del dato — no es lo mismo la posicion tecleada de
                una celda que la que acaba de dar el GPS. */
             pos_texto().c_str(),
             gps_texto().c_str(),
             red_texto().c_str());
    /* Y AQUI LA COMPROBACION QUE FALTABA. `snprintf` devuelve lo que HABRIA
       escrito, no lo que escribio: si eso supera el buffer, la linea salio
       cortada. Mejor un aviso feo por el cable que perder campos sin enterarse.
       Por el cable y no por todos lados: si el estado esta roto, mandar el
       aviso por el mismo sitio no ayudaria. */
    if (strlen(s) >= sizeof s - 1)
        log_usb("⚠ la linea de estado NO CABE en su buffer: hay campos cortados");
    kiss_manda(EV_ESTADO, (const uint8_t *)s, strlen(s));
}

/* OJO con el tipo de `n`: es uint16_t y NO uint8_t. Lo fue durante un rato y
   costo carisimo: cualquier trozo de mas de 255 bytes se truncaba en silencio
   (512 -> 0, 258 -> 2), asi que la actualizacion por Bluetooth escribia cero
   bytes y confirmaba igual. Sintomas: fallos en puntos distintos cada vez, sin
   reinicios y sin un solo error. */
static void orden(uint8_t tipo, uint8_t *d, uint16_t n, int idx)
{
    Tubo &tu = tubos[idx];
    hay_anfitrion = true;
    tu.t_ultimo = millis();

    /* Por el enlace entre nodos no viajan ordenes, viajan tramas del aire: se
       atiende aparte y no pasa por nada de lo de abajo. */
    if (tu.clase == TUBO_ENLACE) { del_enlace(tipo, d, n, idx); return; }

    /* La puerta. Sin autorizar, lo unico que se atiende es el codigo; todo lo
       demas se tira sin contemplaciones.

       El origen se sabe con certeza porque cada tubo tiene su propio
       desentramado (ver leer_serie). Antes se deducia de `bt_conectado` y no
       funcionaba: una orden por radio se colaba como si viniera del cable. */
    if (!tu.autorizado && !pedir_codigo) tu.autorizado = true;
    if (!tu.autorizado) {
        if (tipo == CMD_CONFIRMA) {
            char metido[8] = {0};
            uint8_t li = n > 6 ? 6 : (uint8_t)n;
            memcpy(metido, d, li);
            if ((uint32_t)atoi(metido) == bt_codigo && li == 6) {
                tu.autorizado = true;
                /* NADA DE NVS CON LA SESION ABIERTA.
                 *
                 * Aqui se llamaba a `apunta_conocido()`, que escribe en la
                 * NVS — y escribir en la NVS **rompe la sesion SPP en curso**
                 * (ya nos costo un diagnostico entero con el codigo por
                 * Bluetooth). El resultado, visto por el usuario: metes el
                 * codigo bueno, el enlace se cae en ese mismo instante, la app
                 * reconecta, el nodo abre sesion NUEVA y genera OTRO codigo, y
                 * el que acabas de escribir ya no vale. Cinco veces seguidas.
                 * Parece un codigo que no se acepta y es un apunte que mata la
                 * conexion justo al aceptarlo.
                 * Se apunta en RAM ahora y en la NVS **al cerrarse la sesion**,
                 * que es cuando ya no hay nada que romper. */
                if (tu.clase == TUBO_BT) {
                    bt_autorizado = true;
                    memcpy(bt_por_apuntar, bt_par, 6);
                    hay_que_apuntar = true;
                }
                redibujar = true;
                kiss_a(tu, EV_LOG, (const uint8_t *)"autorizado", 10);
                manda_estado();
            } else {
                /* Por el CABLE se dice exactamente que llego y que se
                   esperaba. Sin esto, "codigo incorrecto" es un callejon sin
                   salida: no se sabe si el usuario mira otra pantalla, si el
                   codigo cambio por una reconexion o si llega con basura
                   pegada. Nunca por el Bluetooth, que es justo el canal que
                   se esta autorizando. */
                char m[80];
                snprintf(m, sizeof m,
                         "codigo NO vale: llego \"%s\" (%u car.), esperaba %06u",
                         metido, (unsigned)n, (unsigned)bt_codigo);
                log_usb(m);
                kiss_a(tu, EV_LOG, (const uint8_t *)"codigo incorrecto", 17);
            }
            return;
        }
        // Se avisa de que hace falta el codigo, sin decir cual: el codigo sale
        // por la pantalla del nodo, no por el canal que se esta autorizando.
        kiss_a(tu, EV_EMPAREJA, (const uint8_t *)"codigo", 6);
        return;
    }
    uint8_t b[CAB_LEN + MAX_PAYLOAD];

    switch (tipo) {
    /* Un cliente puede declarar SU indicativo. Sin esto, con varios usuarios
       colgados del mismo nodo todos emitirian bajo el indicativo del nodo, que
       en radio de aficionado no vale: cada estacion se identifica. */
    case CMD_IDENT: {
        uint8_t li = n > MAX_INDICATIVO ? MAX_INDICATIVO : (uint8_t)n;
        memcpy(tu.indicativo, d, li);
        tu.indicativo[li] = 0;
        tu.src = li ? hash_indicativo(tu.indicativo) : 0;
        redibujar = true;
        manda_estado();
        break;
    }
    case CMD_INICIO: {
        /* Y si esta hablando otro por el canal, tampoco se le pisa.
           Vale para la red ENTERA, no solo para el aire de aqui: lo que llega
           por un enlace pasa por el mismo camino de recepcion, asi que tambien
           marca el canal como ocupado. Se comprueba EN EL NODO y no solo en la
           app, por lo mismo que los limites de la radio: asi protege de
           cualquier cliente, no solo del nuestro.
           Ojo, esto no elimina las colisiones, solo las que se pueden ver: dos
           que pulsen con menos de un tiempo de ida de diferencia (~74 ms de
           LoRa mas lo que tarde Internet) seguiran pisandose, igual que en
           cualquier repetidor. */
        if (canal_ocupado && ptt_de != idx) {
            uint8_t m[1 + MAX_INDICATIVO];
            m[0] = PTT_DE_OTRO;
            uint8_t li = strlen(ultimo_ind);
            if (li > MAX_INDICATIVO) li = MAX_INDICATIVO;
            memcpy(m + 1, ultimo_ind, li);
            kiss_a(tu, EV_PTT, m, 1 + li);
            break;
        }
        // El microfono es de uno solo: el canal es simplex y la radio, una.
        if (!coge_ptt(idx)) break;
        tx_stream = random(1, 256);
        tx_seq = 0;
        transmitiendo = true;
        redibujar = true;
        despierta_pantalla();
        // El indicativo y el origen son los del CLIENTE, no los del nodo.
        const char *quien = indicativo_de(idx);
        cabecera(b, T_INICIO, saltos_def, src_de(idx), tx_stream, 0);
        uint8_t l = CAB_LEN;
        b[l++] = n > 0 ? d[0] : C2_1200;               // modo de codec
        uint8_t li = strlen(quien);
        memcpy(b + l, quien, li); l += li;
        /* Y LA POSICION DETRAS, tras un `\0` (v1.35). Aqui es donde sale la de
           una persona: cuando se identifica de todas formas al abrir el PTT.
           Nueve bytes en una trama que ya iba a salir, en vez de una baliza mas
           cada minuto diciendo por donde anda.
           Mismo truco de compatibilidad que el nombre en la baliza: quien no lo
           entienda corta el indicativo en el primer cero y lee lo de siempre. */
        if (pos_vale() && pos_publica) {
            b[l++] = 0;
            uint32_t la = (uint32_t)pos_lat, lo = (uint32_t)pos_lon;
            for (int i = 0; i < 4; i++) b[l++] = (uint8_t)(la >> (8 * i));
            for (int i = 0; i < 4; i++) b[l++] = (uint8_t)(lo >> (8 * i));
        }
        apuntar(src_de(idx), tx_stream, 0, T_INICIO);
        emitir(b, l);
        // Eco local: los demas clientes de ESTE nodo no lo oirian por radio.
        entrega_al_anfitrion(b, l, 127, 0, idx);
        reparte_al_enlace(b, l, -1);
        break;
    }
    case CMD_VOZ: {
        if (!transmitiendo || ptt_de != idx) break;
        if (n > MAX_PAYLOAD) n = MAX_PAYLOAD;
        cabecera(b, T_VOZ, saltos_def, src_de(idx), tx_stream, ++tx_seq);
        memcpy(b + CAB_LEN, d, n);
        apuntar(src_de(idx), tx_stream, tx_seq, T_VOZ);
        emitir(b, CAB_LEN + n);
        entrega_al_anfitrion(b, CAB_LEN + n, 127, 0, idx);
        reparte_al_enlace(b, CAB_LEN + n, -1);
        break;
    }
    case CMD_FIN: {
        if (ptt_de != idx) break;
        if (transmitiendo) {
            transmitiendo = false;
            redibujar = true;
            cabecera(b, T_FIN, saltos_def, src_de(idx), tx_stream, ++tx_seq);
            apuntar(src_de(idx), tx_stream, tx_seq, T_FIN);
            emitir(b, CAB_LEN);
            entrega_al_anfitrion(b, CAB_LEN, 127, 0, idx);
            reparte_al_enlace(b, CAB_LEN, -1);
        }
        ptt_de = -1;
        avisa_ptt();
        break;
    }
    case CMD_CONFIG: {
        if (n >= 2) {
            mi_canal = d[0];
            saltos_def = d[1] & 0x0F;
        }
        // Byte 2 opcional: potencia en dBm (2-17). Sirve para probar dos placas
        // en la misma mesa sin saturarse mutuamente el receptor.
        uint8_t desde = 2;
        if (n >= 4 && d[3] <= PERFIL_SOLO) {
            perfil = d[3];
            redibujar = true;
        }
        if (n >= 5 && d[4] <= PANTALLA_OFF) {
            pantalla_modo = d[4];
            despierta_pantalla();
        }
        if (n >= 3 && d[2] >= 2 && d[2] <= 17) {
            potencia = d[2];
            aplica_radio();
            // IMPRESCINDIBLE: `setOutputPower()` deja el chip en STANDBY y no
            // vuelve solo a recepcion. Sin este re-armado, el nodo se quedaba
            // SORDO en cuanto se le configuraba — y como la prueba mandaba la
            // configuracion antes de empezar, no recibia nunca. Vale para
            // cualquier cambio de parametros de radio, no solo la potencia.
            st_rx = armar_rx();
            desde = (n >= 4 && d[3] <= PERFIL_SOLO) ? 4 : 3;
            if (n >= 5 && d[4] <= PANTALLA_OFF) desde = 5;
        }
        if (n > desde) {
            uint8_t li = (uint8_t)(n - desde);
            if (li > MAX_INDICATIVO) li = MAX_INDICATIVO;
            memcpy(mi_indicativo, d + desde, li);
            mi_indicativo[li] = 0;
            /* El `src` NO cambia al cambiar el indicativo: ver `src_de_la_mac`. */
            renombra_bt();
        }
        guarda_ajustes();
        manda_estado();
        break;
    }
    case CMD_ESTADO:
        manda_estado();
        break;

    case CMD_CONFIRMA:
        // Si se llega aqui es que ya estaba autorizado (o viene por USB). Con
        // "olvidar" de payload se limpia la lista de moviles.
        if (n >= 7 && memcmp(d, "olvidar", 7) == 0) olvida_conocidos();
        else if (n >= 9 && memcmp(d, "codigo si", 9) == 0) {
            pedir_codigo = true; guarda_ajustes();
            log_txt("se pedira codigo en las proximas conexiones");
        } else if (n >= 9 && memcmp(d, "codigo no", 9) == 0) {
            pedir_codigo = false; guarda_ajustes();
            log_txt("no se pedira codigo");
        } else log_txt("ya autorizado");
        break;

    /* --- actualizacion por Bluetooth --- */
    case CMD_OTA_INI: {
        if (transmitiendo) { log_txt("no se actualiza mientras se transmite"); break; }
        if (n < 4) { log_txt("CMD_OTA_INI mal formado"); break; }
        uint32_t tam = (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 |
                       (uint32_t)d[2] << 8 | d[3];
        /* ⛔ SIEMPRE LIMPIAR ANTES DE EMPEZAR, Y ESTO CASI DEJA UN NODO MUERTO.
         *
         * `Update.begin()` devuelve false —**con `getError()` a 0**, o sea sin
         * decir nada— si ya hay una actualizacion abierta. Y se puede quedar
         * abierta muy facilmente: basta con que al emisor se le corte la
         * conexion a media transferencia.
         *
         * Lo grave es que `caduca_ota()` NO lo rescataba: mira `ota_curso`, que
         * es NUESTRA bandera, y esa si se limpia cuando `begin()` falla. Las dos
         * se desincronizan y desde ese momento **el nodo no acepta ninguna
         * actualizacion mas hasta que alguien lo reinicie fisicamente**.
         * Medido el 8-sep-2026 con nodo-de-casa: tres intentos seguidos por WiFi y
         * a partir del segundo, "no cabe la actualizacion: error 0" para
         * siempre.
         *
         * En una celda en un tejado eso es justo el desastre que no te puedes
         * permitir: un intento fallido y hay que subir a desenchufarla. Pedir
         * una actualizacion nueva SIGNIFICA "empieza de cero", asi que se
         * aborta lo que hubiera sin preguntar. */
        if (Update.isRunning()) {
            Update.abort();
            log_txt("habia una actualizacion a medias: se descarta y se empieza de cero");
        }
        if (!Update.begin(tam)) {
            char m[64];
            snprintf(m, sizeof m, "no cabe la actualizacion (%u B): error %d",
                     (unsigned)tam, Update.getError());
            log_txt(m);
            break;
        }
        if (n >= 4 + 32) {
            char md5[33] = {0};
            memcpy(md5, d + 4, 32);
            Update.setMD5(md5);        // se verifica al cerrar: si no cuadra, no arranca
        }
        ota_bytes = 0;
        ota_total = tam;
        ota_curso = true;
        ota_idx = 0;
        ota_visto = millis();
        /* Fuera el watchdog mientras dure. Escribir en flash con el Bluetooth
           activo bloquea lo bastante como para que salte, y el nodo se
           reiniciaba a media actualizacion (comprobado: se reinicio en el byte
           23040). Es lo mismo que hace la actualizacion por WiFi. */
        esp_task_wdt_delete(NULL);
        /* Y se duerme la radio. No es solo ahorro: sus interrupciones y el
           trafico SPI conviviendo con las escrituras en flash reiniciaban el
           nodo a media actualizacion (llegaba al 18%). Ademas no se puede
           hablar mientras te actualizas, asi que no se pierde nada. */
        radio.sleep();
        pantalla_modo_previo = pantalla_modo;
        despierta_pantalla();
        log_txt("actualizacion por Bluetooth iniciada");
        break;
    }

    case CMD_OTA_DAT: {
        if (!ota_curso) break;
        if (n < 3) break;
        uint16_t idx = (uint16_t)d[0] << 8 | d[1];
        d += 2; n -= 2;
        if (idx != ota_idx) {
            /* No es el que toca. Si es el anterior, el emisor no vio nuestro
               acuse: se le repite y ya esta. Si es otro, se ignora y el emisor
               acabara reenviando el que falta. */
            if (idx + 1 == ota_idx) {
                uint8_t pct = ota_total ? (uint8_t)(ota_bytes * 100ULL / ota_total) : 0;
                uint8_t r[3] = { pct, (uint8_t)(ota_idx >> 8), (uint8_t)ota_idx };
                kiss_manda(EV_OTA, r, 3);
            }
            break;
        }
        if (Update.write(d, n) != n) {
            char m[48];
            snprintf(m, sizeof m, "fallo escribiendo (error %d)", Update.getError());
            log_txt(m);
            Update.abort();
            ota_curso = false;
            esp_task_wdt_add(NULL);
            st_rx = armar_rx();
            break;
        }
        ota_bytes += n;
        ota_idx++;
        ota_visto = millis();
        /* Se confirma CADA trozo. Antes solo se avisaba cada 32 kB y el emisor
           tenia que adivinar el ritmo con una pausa fija: escribir en flash es
           mucho mas lento que recibir por SPP, el buffer se llenaba y la
           transferencia se atascaba sin dar ningun error. Con acuse por trozo
           el emisor va exactamente al ritmo del nodo. */
        {
            // El acuse lleva el numero del PROXIMO trozo esperado: asi el
            // emisor sabe exactamente por donde seguir si algo se pierde.
            uint8_t pct = ota_total ? (uint8_t)(ota_bytes * 100ULL / ota_total) : 0;
            uint8_t r[3] = { pct, (uint8_t)(ota_idx >> 8), (uint8_t)ota_idx };
            kiss_manda(EV_OTA, r, 3);
        }
        if (ota_bytes / 65536 != (ota_bytes - n) / 65536) redibujar = true;
        break;
    }

    case CMD_OTA_FIN: {
        if (!ota_curso) break;
        ota_curso = false;
        if (Update.end(true)) {
            log_txt("actualizacion correcta; reiniciando");
            delay(400);
            ESP.restart();
        } else {
            char m[64];
            snprintf(m, sizeof m, "actualizacion rechazada: error %d",
                     Update.getError());
            log_txt(m);          // el firmware viejo sigue intacto
            esp_task_wdt_add(NULL);
            st_rx = armar_rx();
        }
        break;
    }

    case CMD_RADIO: {
        if (n < 9) { log_txt("CMD_RADIO necesita 9 bytes"); break; }
        float f = ((uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 |
                   (uint32_t)d[2] << 8 | d[3]) / 1000.0f;
        float bw = ((uint16_t)d[4] << 8 | d[5]);
        uint8_t nsf = d[6], ncr = d[7], npot = d[8];
        // Se valida AQUI, no en la app: el nodo es el que sabe lo que aguanta
        // su hardware, y ademas asi no depende de que el cliente sea el nuestro.
        if (f < FREQ_MIN || f > FREQ_MAX || nsf < 6 || nsf > 12 ||
            ncr < 5 || ncr > 8 || npot < POT_MIN || npot > POT_MAX) {
            log_txt("ajuste fuera de los limites del hardware");
            manda_estado();
            break;
        }
        /* Y ademas, la BANDA. El SX1278 llega a 520 MHz; la licencia no. Esto
           no protege de nadie decidido —el firmware es abierto y cambiar
           `BANDA_MIN`/`BANDA_MAX` y recompilar son dos minutos— pero si evita
           lo unico que iba a pasar de verdad: un dedo torpe escribiendo 443 en
           vez de 434 y transmitiendo fuera de banda sin enterarse.
           Se comprueba en el nodo y no solo en la app porque cualquiera puede
           mandar un CMD_RADIO: `nodo.py`, un script, otro cliente. */
        if (f < BANDA_MIN || f > BANDA_MAX) {
            log_txt("fuera de la banda de aficionados: no se aplica");
            manda_estado();
            break;
        }
        float f0 = frecuencia, b0 = ancho; uint8_t s0 = sf, c0 = cr, p0 = potencia;
        frecuencia = f; ancho = bw; sf = nsf; cr = ncr; potencia = npot;
        if (!aplica_radio()) {          // vuelta atras si el chip lo rechaza
            frecuencia = f0; ancho = b0; sf = s0; cr = c0; potencia = p0;
            aplica_radio();
        } else {
            guarda_ajustes();
        }
        manda_estado();
        break;
    }

    case CMD_WIFI: {
        // [ssid]\0[clave]. Sin datos = olvidar el WiFi.
        // Se conserva por compatibilidad con la app 0.5.0: equivale a CMD_RED
        // en modo cliente.
        wifi_ssid = ""; wifi_clave = "";
        uint16_t i = 0;
        while (i < n && d[i] != 0) { wifi_ssid += (char)d[i]; i++; }
        i++;
        while (i < n) { wifi_clave += (char)d[i]; i++; }
        red_modo = wifi_ssid.length() ? RED_CLIENTE : RED_OFF;
        guarda_ajustes();
        log_txt(wifi_ssid.length()
                ? "WiFi guardado; tiene efecto al reiniciar el nodo"
                : "WiFi olvidado");
        manda_estado();
        break;
    }

    /* [modo][ssid]\0[clave].
       El modo punto de acceso es lo que hace util el multiusuario sin
       infraestructura: el nodo va en la mochila, levanta su propia red y el
       grupo se cuelga de el. No hay Internet, ni router, ni nada. */
    case CMD_RED: {
        if (n < 1) { log_txt("CMD_RED necesita el modo"); break; }
        uint8_t m = d[0];
        if (m > RED_AP) { log_txt("modo de red desconocido"); break; }
        String ss, cl;
        uint16_t i = 1;
        while (i < n && d[i] != 0) { ss += (char)d[i]; i++; }
        i++;
        while (i < n) { cl += (char)d[i]; i++; }
        /* Un punto de acceso ABIERTO seria dejar el mando del nodo a quien pase
           por la calle: cualquiera se conecta, transmite con su indicativo y le
           cambia los ajustes. Por eso en modo AP la clave es la puerta, y sin
           ella no se levanta. */
        if (m == RED_AP && cl.length() < 8) {
            log_txt("el punto de acceso necesita una clave de 8 caracteres o mas");
            break;
        }
        if (m == RED_CLIENTE && ss.length() == 0) {
            log_txt("falta el nombre de la red");
            break;
        }
        red_modo = m; wifi_ssid = ss; wifi_clave = cl;
        guarda_ajustes();
        log_txt("red guardada; tiene efecto al reiniciar el nodo");
        manda_estado();
        break;
    }

    /* [puerto:2][host] AÑADE un destino; sin datos, los suelta todos.
       Se puede llamar varias veces para encadenar ubicaciones. */
    /* [pin][previo_ms][cola_ms][preambulo] — amplificador externo. */
    case CMD_AMPLI: {
        if (n < 1) { log_txt("CMD_AMPLI necesita al menos el pin"); break; }
        uint8_t np = d[0];
        /* Pines que NO se pueden usar y por que, en esta placa:
           - los que ya tiene la radio, la pantalla y (en la T-Beam) el PMU;
           - GPIO16 en la LoRa32, que **cuelga el sistema** (ver la nota de la
             pantalla, costo un arranque en bucle);
           - 34-39 son SOLO ENTRADA en el ESP32: aceptarlos como salida seria
             dejar al usuario con una linea de PTT que nunca sube. */
        if (np != 0 && (np > 33 || np == 16 ||
                        np == P_SCK || np == P_MISO || np == P_MOSI ||
                        np == P_CS  || np == P_RST  || np == P_DIO0 ||
                        np == 21 || np == 22)) {
            log_txt("ese pin no se puede usar para el PTT");
            break;
        }
        if (pin_ptt && pin_ptt != np) digitalWrite(pin_ptt, LOW);
        pin_ptt = np;
        if (n > 1) ptt_previo = d[1];
        if (n > 2) ptt_cola   = d[2];
        if (n > 3) preambulo  = d[3] < 6 ? 6 : d[3];   // el minimo del chip es 6
        if (pin_ptt) { pinMode(pin_ptt, OUTPUT); digitalWrite(pin_ptt, LOW); }
        guarda_ajustes();
        aplica_radio();                 // el preambulo se aplica aqui (y rearma RX)
        char m[80];
        snprintf(m, sizeof m, "amplificador: pin %u, %u ms antes, %u despues, "
                 "preambulo %u", pin_ptt, ptt_previo, ptt_cola, preambulo);
        log_txt(m);
        manda_estado();
        break;
    }

    /* [ssid]\0[clave] — segunda red. Vacio = olvidarla. */
    case CMD_RED2: {
        wifi_ssid2 = ""; wifi_clave2 = "";
        uint16_t i = 0;
        while (i < n && d[i] != 0) { wifi_ssid2 += (char)d[i]; i++; }
        i++;
        while (i < n) { wifi_clave2 += (char)d[i]; i++; }
        guarda_ajustes();
        log_txt(wifi_ssid2.length()
                ? "segunda red guardada; tiene efecto al reiniciar el nodo"
                : "segunda red olvidada");
        manda_estado();
        break;
    }

    /* [puerto:2][host ASCII] — canal de mando saliente. Vacio = soltarlo. */
    case CMD_MANDO: {
        if (n == 0) {
            mando_host = ""; mando_puerto = 0;
            if (mando_tubo >= 0) { cierra_tubo(mando_tubo); mando_tubo = -1; }
            guarda_ajustes();
            log_txt("canal de mando soltado");
            manda_estado();
            break;
        }
        if (n < 3) { log_txt("CMD_MANDO necesita [puerto:2][host]"); break; }
        mando_puerto = ((uint16_t)d[0] << 8) | d[1];
        mando_host = "";
        for (uint16_t i = 2; i < n; i++) mando_host += (char)d[i];
        if (mando_tubo >= 0) { cierra_tubo(mando_tubo); mando_tubo = -1; }
        t_mando = 0;                       // que lo intente en la vuelta siguiente
        guarda_ajustes();
        char m[80];
        snprintf(m, sizeof m, "canal de mando: %s:%u",
                 mando_host.c_str(), (unsigned)mando_puerto);
        log_txt(m);
        manda_estado();
        break;
    }

    /* [nombre ASCII] — nombre del cacharro para la lista del movil. Vacio =
       volver al automatico `nodo-XXXX`. Tiene efecto al reiniciar (el nombre
       Bluetooth se fija al arrancar: ver `renombra_bt`). */
    case CMD_BT: {
        if (n < 1) break;
        uint8_t m = d[0] > BT_SIEMPRE ? BT_SIEMPRE : d[0];
        bt_modo = m;
        guarda_ajustes();
        char msg[96];
        snprintf(msg, sizeof msg,
                 "Bluetooth: %s (tiene efecto al reiniciar el nodo)",
                 m == BT_APAGADO ? "apagado" :
                 (m == BT_VENTANA ? "ventana de 5 min tras arrancar" : "siempre encendido"));
        log_txt(msg);
        manda_estado();
        break;
    }

    /* POSICION DEL NODO. Vacio = olvidarla.
     *
     * Se comprueba el rango: una latitud de 200 grados no es un nodo mal
     * puesto, es un fallo de quien manda —o un byte movido— y publicarla
     * ensucia el mapa de todos. Mas vale rechazarla aqui, que es donde se sabe.
     *
     * ⚠️ La FIJA escribe en NVS y la VIVA no, y por eso la viva no lleva
     * `manda_estado()` en cada llegada: el movil puede mandar su GPS cada dos
     * segundos y devolverle 500 bytes de estado por cada uno seria gastar el
     * enlace en nada. */
    case CMD_POS: {
        if (n == 0) {
            /* Vacio = OLVIDARLA Y CALLARSE, y lo segundo es lo importante desde
               que un nodo puede saber donde esta el solo: sin `pos_publica`, el
               GPS volveria a ponerla en la siguiente trama de NMEA y el
               interruptor de la app no gobernaria nada. Se guarda en NVS porque
               un nodo desatendido tiene que seguir callado tras un corte. */
            hay_pos = false;
            pos_publica = false;
            prefs.begin("pttlora", false);
            prefs.remove("poslat"); prefs.remove("poslon");
            prefs.putBool("pospub", false);
            prefs.end();
            log_txt("posicion olvidada; este nodo deja de publicarla");
            manda_estado();
            break;
        }
        if (n < 9) break;
        int32_t la = (int32_t)((uint32_t)d[1] | ((uint32_t)d[2] << 8) |
                               ((uint32_t)d[3] << 16) | ((uint32_t)d[4] << 24));
        int32_t lo = (int32_t)((uint32_t)d[5] | ((uint32_t)d[6] << 8) |
                               ((uint32_t)d[7] << 16) | ((uint32_t)d[8] << 24));
        if (la < -900000000 || la > 900000000 ||
            lo < -1800000000 || lo > 1800000000) {
            log_txt("posicion fuera del planeta: no se aplica");
            break;
        }
        bool fija = (d[0] == POS_FIJA);
        /* Que le manden una posicion vuelve a encender la publicacion: el
           interruptor de la app es una sola orden en los dos sentidos. */
        if (!pos_publica) {
            pos_publica = true;
            prefs.begin("pttlora", false);
            prefs.putBool("pospub", true);
            prefs.end();
        }
        /* PRECEDENCIA. Una posicion prestada por un movil no pisa ni la
           tecleada —que es una decision— ni la del GPS del propio aparato
           mientras este fresca: el dato de uno mismo vale mas que el de al
           lado, y sobre todo NO PUEDE BAILAR, porque de que origen sea depende
           como se emite (baliza ciclica o solo al transmitir). */
        if (!fija && hay_pos) {
            if (pos_origen == POS_FIJA) break;
            if (pos_origen == POS_GPS &&
                (uint32_t)(millis() - t_pos) < GPS_MANDA_MS) break;
        }
        pos_lat = la; pos_lon = lo; hay_pos = true;
        pos_origen = fija ? POS_FIJA : POS_VIVA;
        t_pos = millis();
        if (fija) {
            prefs.begin("pttlora", false);
            prefs.putInt("poslat", pos_lat);
            prefs.putInt("poslon", pos_lon);
            prefs.end();
            char m[64];
            snprintf(m, sizeof m, "posicion fija %.6f,%.6f",
                     pos_lat / 1e7, pos_lon / 1e7);
            log_txt(m);
            manda_estado();
        }
        break;
    }

    case CMD_NOMBRE: {
        uint8_t li = n > (sizeof nombre_nodo - 1) ? (sizeof nombre_nodo - 1) : (uint8_t)n;
        memcpy(nombre_nodo, d, li);
        nombre_nodo[li] = 0;
        guarda_ajustes();
        char m[64];
        snprintf(m, sizeof m, "nombre \"%s\"; el Bluetooth lo toma al reiniciar",
                 nombre_nodo);
        log_txt(m);
        manda_estado();
        break;
    }

    case CMD_ENLACE: {
        if (n == 0) {
            for (int i = 0; i < SALIENTES_N; i++) {
                enlace_host[i] = ""; enlace_puerto[i] = 0;
            }
            for (int i = T_ENL0; i < TUBOS_N; i++)
                if (tubos[i].clase == TUBO_ENLACE) cierra_tubo(i);
            guarda_ajustes();
            log_txt("enlaces soltados");
            manda_estado();
            break;
        }
        if (n < 3) { log_txt("CMD_ENLACE necesita [puerto:2][host]"); break; }
        uint16_t pto = ((uint16_t)d[0] << 8) | d[1];
        String h;
        for (uint16_t i = 2; i < n; i++) h += (char)d[i];
        int hueco = -1;
        for (int i = 0; i < SALIENTES_N; i++) {
            if (enlace_host[i] == h && enlace_puerto[i] == pto) { hueco = i; break; }
            if (hueco < 0 && enlace_host[i].length() == 0) hueco = i;
        }
        if (hueco < 0) {
            log_txt("ya hay dos destinos; suelta los enlaces antes de cambiarlos");
            break;
        }
        enlace_host[hueco] = h;
        enlace_puerto[hueco] = pto;
        t_enlace = 0;                    // que lo intente ya, sin esperar
        guarda_ajustes();
        manda_estado();
        break;
    }

    /* Autoprueba del watchdog: se cuelga a proposito. Si el watchdog esta
       vivo, la placa se reinicia sola a los ~10 s; si no vuelve, es que no lo
       esta y NO conviene dejar ese nodo en un poste. Merece la pena
       comprobarlo antes de subir a instalar algo que no vas a poder tocar. */
    case 0x07:
        kiss_manda(EV_LOG, (const uint8_t *)"colgando a proposito: la placa "
                   "debe reiniciarse sola en ~10 s", 61);
        delay(100);
        while (true) { }
        break;

    // Diagnostico: recepcion BLOQUEANTE, sin interrupcion de por medio. Sirve
    // para distinguir "la radio no oye" de "la interrupcion no salta".
    case 0x06: {
        char msg[96];
        uint8_t buf[CAB_LEN + MAX_PAYLOAD];
        int st = radio.receive(buf, 0);
        snprintf(msg, sizeof msg, "diag receive() st=%d len=%d rssi=%.0f snr=%.1f",
                 st, (int)radio.getPacketLength(), radio.getRSSI(), radio.getSNR());
        kiss_manda(EV_ESTADO, (const uint8_t *)msg, strlen(msg));
        st_rx = armar_rx();
        break;
    }
    }
}

/* Desentrama KISS byte a byte, con un estado POR TUBO.
   Se hace asi y no leyendo lineas porque el audio es binario y cualquier byte
   puede aparecer, incluido el 0x0A. Y separado por tubo porque hace falta saber
   con certeza POR DONDE entro cada orden: de eso dependen la autorizacion y,
   ahora, con que indicativo se emite. */
static void mastica(int idx, uint8_t c)
{
    Tubo &t = tubos[idx];
    if (c == KISS_FEND) {
        if (t.dentro && t.n >= 1) orden(t.buf[0], t.buf + 1, t.n - 1, idx);
        t.dentro = true; t.escape = false; t.n = 0;
        return;
    }
    if (!t.dentro) return;
    if (c == KISS_FESC) { t.escape = true; return; }
    if (t.escape) { c = (c == KISS_TFEND) ? KISS_FEND : KISS_FESC; t.escape = false; }
    if (t.n < t.cap) t.buf[t.n++] = c;
}

/* Cierra un tubo y deja el nodo consistente.
   Lo importante es lo ultimo: si el que se va tenia el PTT hay que soltarlo, o
   un movil que se queda sin cobertura con el boton apretado deja mudo al resto
   del grupo hasta que caduque el TOT. */
static void cierra_tubo(int idx)
{
    Tubo &t = tubos[idx];
    if (t.clase == TUBO_TCP || t.clase == TUBO_ENLACE) t.cli.stop();
    t.clase = TUBO_LIBRE;
    t.autorizado = false;
    t.indicativo[0] = 0;
    t.src = 0;
    t.n = 0; t.dentro = false; t.escape = false;
    t.salida_k = -1;
    if (ptt_de == idx) suelta_ptt("se fue el que hablaba");
    redibujar = true;
}

static void leer_serie()
{
    while (Serial.available())
        mastica(T_USB, (uint8_t)Serial.read());
    /* Del anillo que llena `bt_datos`, y NO sondeando la caracteristica: con un
       callback de datos puesto, el core ya NO alimenta su cola interna. */
    while (bt_cola != bt_cabeza) {
        uint8_t b = bt_anillo[bt_cola];
        bt_cola = (uint16_t)((bt_cola + 1) % BT_ANILLO);
        mastica(T_BT, b);
        bt_ultimo = millis();
        bt_hablo = true;        // ya no es una sesion fantasma
    }
    /* Clientes por WiFi. Se leen hasta 256 bytes por vuelta y cliente: mucho
       mas que un lote de voz, y poco bastante como para que un cliente que
       vuelca datos no deje al resto del nodo (radio incluida) sin atender. */
    for (int i = T_TCP0; i < TUBOS_N; i++) {
        Tubo &t = tubos[i];
        if (t.clase != TUBO_TCP && t.clase != TUBO_ENLACE) continue;
        if (!t.cli.connected()) { cierra_tubo(i); continue; }
        int cuantos = 0;
        while (t.cli.available() && cuantos++ < 256) {
            mastica(i, (uint8_t)t.cli.read());
            t.t_ultimo = millis();
        }
    }
}


// ------------------------------------------------------------- ajustes ----
/* El indicativo es identificacion de estacion: perderlo en cada reinicio y
   volver a "NOCALL" no es una molestia, es emitir sin identificar. Se guarda en
   NVS junto al resto de la configuracion. */
static void carga_ajustes()
{
    prefs.begin("pttlora", true);
    String ind   = prefs.getString("ind", "");
    mi_canal     = prefs.getUChar("canal", 1);
    saltos_def   = prefs.getUChar("saltos", SALTOS_DEF);
    potencia     = prefs.getUChar("pot", CANAL_POTENCIA);
    perfil       = prefs.getUChar("perfil", PERFIL_AUTO);
    bt_modo      = prefs.getUChar("btmodo", BT_SIEMPRE);
    pantalla_modo = prefs.getUChar("pant", PANTALLA_AUTO);
    pedir_codigo  = prefs.getBool("codigo", false);
    frecuencia   = prefs.getFloat("frec", CANAL_MHZ);
    /* Una frecuencia guardada en NVS por un firmware anterior —o por uno con
       otra banda compilada— no puede sacar al nodo de banda al arrancar. Si el
       valor guardado no vale, se vuelve al canal de fabrica. */
    if (frecuencia < BANDA_MIN || frecuencia > BANDA_MAX) frecuencia = CANAL_MHZ;
    ancho        = prefs.getFloat("bw", CANAL_BW_KHZ);
    sf           = prefs.getUChar("sf", CANAL_SF);
    cr           = prefs.getUChar("cr", CANAL_CR);
    pin_ptt      = prefs.getUChar("pttpin", 0);
    ptt_previo   = prefs.getUChar("pttpre", 10);
    ptt_cola     = prefs.getUChar("pttcol", 5);
    preambulo    = prefs.getUChar("preamb", 8);
    prefs.getString("nombre", nombre_nodo, sizeof nombre_nodo);
    /* Solo la FIJA se guarda: una celda tiene que saber donde esta despues de
       un corte de luz y sin nadie delante. La viva no se guarda a proposito
       (ver CMD_POS), asi que un nodo movil arranca sin posicion y espera a su
       GPS o a su movil, que es lo correcto: donde estaba ayer no dice nada. */
    pos_lat = prefs.getInt("poslat", 0);
    pos_lon = prefs.getInt("poslon", 0);
    if (pos_lat || pos_lon) { hay_pos = true; pos_origen = POS_FIJA; }
    /* Si el usuario apago la posicion, el nodo sigue callado tras un reinicio.
       Por defecto SI publica: un aparato con GPS que se pone en la red se
       comporta como un tracker, y quien no lo quiera lo apaga una vez. */
    pos_publica = prefs.getBool("pospub", true);
    wifi_ssid    = prefs.getString("ssid", "");
    wifi_ssid2   = prefs.getString("ssid2", "");
    mando_host   = prefs.getString("mndh", "");
    mando_puerto = prefs.getUShort("mndp", 0);
    wifi_clave2  = prefs.getString("wpass2", "");
    wifi_clave   = prefs.getString("wpass", "");
    /* Compatibilidad: los nodos que ya tenian WiFi guardado lo tenian como
       cliente, que era lo unico que habia. */
    red_modo     = prefs.getUChar("red", wifi_ssid.length() ? RED_CLIENTE : RED_OFF);
    for (int i = 0; i < SALIENTES_N; i++) {
        char k[8];
        snprintf(k, sizeof k, "enlh%d", i);
        enlace_host[i] = prefs.getString(k, "");
        snprintf(k, sizeof k, "enlp%d", i);
        enlace_puerto[i] = prefs.getUShort(k, 0);
    }
    prefs.end();
    if (ind.length() > 0 && ind.length() <= MAX_INDICATIVO) {
        strncpy(mi_indicativo, ind.c_str(), MAX_INDICATIVO);
        mi_indicativo[MAX_INDICATIVO] = 0;
    }
    mi_src = src_de_la_mac();
}

static void guarda_ajustes()
{
    prefs.begin("pttlora", false);
    prefs.putString("ind", mi_indicativo);
    prefs.putUChar("canal", mi_canal);
    prefs.putUChar("saltos", saltos_def);
    prefs.putUChar("pot", potencia);
    prefs.putUChar("perfil", perfil);
    prefs.putUChar("btmodo", bt_modo);
    prefs.putUChar("pant", pantalla_modo);
    prefs.putBool("codigo", pedir_codigo);
    prefs.putFloat("frec", frecuencia);
    prefs.putFloat("bw", ancho);
    prefs.putUChar("sf", sf);
    prefs.putUChar("cr", cr);
    prefs.putUChar("pttpin", pin_ptt);
    prefs.putUChar("pttpre", ptt_previo);
    prefs.putUChar("pttcol", ptt_cola);
    prefs.putUChar("preamb", preambulo);
    prefs.putString("nombre", nombre_nodo);
    prefs.putString("ssid", wifi_ssid);
    prefs.putString("ssid2", wifi_ssid2);
    prefs.putString("mndh", mando_host);
    prefs.putUShort("mndp", mando_puerto);
    prefs.putString("wpass2", wifi_clave2);
    prefs.putString("wpass", wifi_clave);
    prefs.putUChar("red", red_modo);
    for (int i = 0; i < SALIENTES_N; i++) {
        char k[8];
        snprintf(k, sizeof k, "enlh%d", i);
        prefs.putString(k, enlace_host[i]);
        snprintf(k, sizeof k, "enlp%d", i);
        prefs.putUShort(k, enlace_puerto[i]);
    }
    prefs.end();
}

// -------------------------------------------------------------- bluetooth --
/* Moviles que ya autorizamos alguna vez: se guardan hasta CONOCIDOS_N en NVS
   para no pedir el codigo cada vez que se conecta el de siempre. */
static bool es_conocido(const uint8_t *bda)
{
    // Espacio de nombres propio, separado de los ajustes: asi la lista de
    // autorizados se puede borrar entera sin tocar la configuracion, y no hay
    // manera de que una clave vieja de otra cosa se cuele como si fuera un
    // movil conocido.
    Preferences a;
    if (!a.begin("pttauth", true)) return false;   // aun no existe: nadie es conocido
    char clave[8];
    bool si = false;
    for (int i = 0; i < CONOCIDOS_N && !si; i++) {
        snprintf(clave, sizeof clave, "p%d", i);
        uint8_t g[6];
        if (a.getBytesLength(clave) == 6 && a.getBytes(clave, g, 6) == 6
            && memcmp(g, bda, 6) == 0) si = true;
    }
    a.end();
    return si;
}

static void apunta_conocido(const uint8_t *bda)
{
    Preferences a;
    a.begin("pttauth", false);
    uint8_t n = a.getUChar("n", 0) % CONOCIDOS_N;
    char clave[8];
    snprintf(clave, sizeof clave, "p%d", n);
    a.putBytes(clave, bda, 6);
    a.putUChar("n", n + 1);
    a.end();
}

/* Olvidar todos los moviles autorizados. Hace falta cuando se pierde un
   telefono o se cede el nodo: si no, el que lo tuviera seguiria entrando. */
/* Aplica los ajustes de radio y REARMA la recepcion.
   Lo del rearmado no es opcional: cualquier cambio de parametros deja el
   SX1278 en standby y no vuelve solo a escuchar. Es exactamente el fallo que
   costo media tarde con `setOutputPower`. */
static bool aplica_radio()
{
    int st = radio.setFrequency(frecuencia);
    if (st == RADIOLIB_ERR_NONE) st = radio.setBandwidth(ancho);
    if (st == RADIOLIB_ERR_NONE) st = radio.setSpreadingFactor(sf);
    if (st == RADIOLIB_ERR_NONE) st = radio.setCodingRate(cr);
    if (st == RADIOLIB_ERR_NONE) st = radio.setOutputPower(potencia);
    if (st == RADIOLIB_ERR_NONE) st = radio.setPreambleLength(preambulo);
    st_rx = armar_rx();
    if (st != RADIOLIB_ERR_NONE) {
        char m[64];
        snprintf(m, sizeof m, "ajuste de radio rechazado (%d)", st);
        log_txt(m);
        return false;
    }
    return true;
}

static void olvida_conocidos()
{
    Preferences a;
    a.begin("pttauth", false);
    a.clear();
    a.end();
    bt_autorizado = false;
    log_txt("olvidados los moviles autorizados");
}

/* OJO: TODO ESTO CORRE EN LA TAREA DE NIMBLE, que tiene poca pila y no admite
   segun que. AQUI NO SE TOCA LA NVS ni se transmite por radio: solo se apunta
   lo minimo en banderas y el trabajo de verdad lo hace el bucle principal
   (`resuelve_conexion_bt`). Es la misma disciplina que ya hacia falta con SPP,
   donde escribir en la NVS desde el callback reventaba la sesion en silencio.

   Se admite UN cliente a la vez, igual que antes, pero por una razon distinta:
   con SPP era una limitacion del chip, aqui es una decision. La autorizacion
   (indicativo, codigo de 6 cifras, movil conocido) esta escrita para un cliente
   y hacerla por conexion es el SIGUIENTE paso, no este. Se implementa dejando
   de anunciarse mientras hay alguien dentro, que es la forma limpia: el segundo
   movil sencillamente no ve el nodo, en vez de conectar y ser expulsado.
   Con esto desaparece `bt_expulsa` y con el todo el lio de "me tiro al otro
   movil". */
class CbServidorBLE : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *srv, ble_gap_conn_desc *desc) override {
        ble_conn = desc->conn_handle;
        memcpy(bt_par, desc->peer_ota_addr.val, 6);
        bt_conectado  = true;
        bt_autorizado = false;
        bt_nuevo      = true;
        bt_hablo      = false;
        /* PARAMETROS DE CONEXION. Los pide el nodo porque el movil, por
           ahorrar bateria, tiende a poner intervalos largos — y con 200 ms de
           intervalo una actualizacion de firmware tarda una eternidad.
           Unidades del estandar, que no son las mismas en cada campo y es
           trampa clasica: intervalo en pasos de 1,25 ms (12=15 ms, 24=30 ms),
           latencia en eventos saltados, supervision en pasos de 10 ms
           (400 = 4 s). Cuatro segundos es lo que tarda el nodo en enterarse de
           que el movil se ha ido sin despedirse; con SPP eso no se detectaba
           NUNCA y de ahi venia todo el andamiaje de fantasmas. */
        srv->updateConnParams(desc->conn_handle, 12, 24, 0, 400);
    }
    void onDisconnect(NimBLEServer *srv) override {
        ble_conn      = 0xFFFF;
        ble_mtu       = 23;
        ble_suscrito  = false;
        bt_conectado  = false;
        bt_autorizado = false;
        bt_nuevo      = false;
        bt_hablo      = false;
        bt_cerrado    = true;   // el tubo se cierra en el bucle, no aqui
        /* Volver a anunciarse. Si esto no se hace, el nodo queda invisible para
           siempre en cuanto el primer movil se desconecta: es EL fallo clasico
           de un servidor BLE y no da ningun error, simplemente no aparece. */
        NimBLEDevice::startAdvertising();
    }
    void onMTUChange(uint16_t mtu, ble_gap_conn_desc *desc) override {
        (void)desc;
        ble_mtu = mtu;
    }
};

/* Notificaciones: si van bien, si el movil se ha suscrito. Ver `ble_escribe`. */
class CbTxBLE : public NimBLECharacteristicCallbacks {
    void onStatus(NimBLECharacteristic *c, Status s, int code) override {
        (void)c; (void)code;
        if (s != SUCCESS_NOTIFY) bt_saltadas++;
    }
    /* EL MOVIL ACTIVA LAS NOTIFICACIONES ESCRIBIENDO SU CCCD, y hasta que no lo
       hace el nodo no puede decirle nada. Es el paso que mas se olvida al
       escribir el lado cliente: se conecta, se descubren los servicios, se
       escribe... y no llega NADA de vuelta, sin ningun error. */
    void onSubscribe(NimBLECharacteristic *c, ble_gap_conn_desc *desc,
                     uint16_t valor) override {
        (void)c; (void)desc;
        ble_suscrito = (valor & 0x0001) != 0;   // bit 0 = notificaciones
    }
};

/* El movil escribe aqui. Solo copiar al anillo: ver `bt_datos`. */
class CbRxBLE : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c) override {
        std::string v = c->getValue();
        if (!v.empty()) bt_datos((const uint8_t *)v.data(), v.size());
    }
};

static CbServidorBLE cb_servidor_ble;
static CbRxBLE       cb_rx_ble;
static CbTxBLE       cb_tx_ble;

/* Aqui si: bucle principal, con toda la pila y sin prisa. */
/* Suelta la ranura si el cliente lleva mucho callado. La app manda un latido
   cada pocos segundos, asi que este silencio solo se da cuando de verdad se ha
   ido. */
/* Una actualizacion que se corta a medias (se va el movil, falla el enlace)
   dejaba el nodo creyendo que sigue en marcha: pantalla de "ACTUALIZANDO" para
   siempre y la particion a medio escribir. Se abandona sola. */
static void caduca_ota()
{
    /* Red de seguridad para la divergencia de arriba: si el objeto `Update`
       sigue abierto pero nosotros ya no creemos estar actualizando, es que se
       quedo colgado. Se cierra, o el nodo no aceptaria ninguna actualizacion
       mas. */
    if (!ota_curso) {
        if (Update.isRunning() && millis() - ota_visto > OTA_MUDO_MS) {
            Update.abort();
            ota_visto = millis();
            log_usb("habia un Update colgado sin actualizacion en curso: cerrado");
        }
        return;
    }
    if (millis() - ota_visto < OTA_MUDO_MS) return;
    Update.abort();
    ota_curso = false;
    esp_task_wdt_add(NULL);
    st_rx = armar_rx();                 // la radio vuelve a escuchar
    redibujar = true;
    log_txt("actualizacion abandonada: se corto a medias");
}

/* Lo mismo para los clientes por WiFi.
   Un movil que se va de cobertura no cierra el socket, y TCP puede tardar
   minutos en enterarse: con ocho ranuras, unos cuantos fantasmas dejan al nodo
   sin sitio para quien si esta. La app manda un latido cada 30 s, asi que tres
   minutos de silencio absoluto solo pasan cuando de verdad se ha ido.
   El enlace NO caduca: por ahi puede no pasar nada durante horas y eso es lo
   normal, no un fallo. */
#define TCP_MUDO_MS 180000UL
static void suelta_tcp_fantasma()
{
    for (int i = T_TCP0; i < T_ENL0; i++) {
        if (tubos[i].clase != TUBO_TCP) continue;
        if (millis() - tubos[i].t_ultimo < TCP_MUDO_MS) continue;
        log_usb("cliente WiFi mudo: se libera la ranura");
        cierra_tubo(i);
    }
}

/* Suelta al cliente que lleva mucho callado.
 *
 * ⚠️ **NO BASTA CON PEDIR LA DESCONEXION Y ESPERAR AL CALLBACK.** Se escribio
 * asi y fue un fallo serio, visto el 8-sep-2026: `disconnect()` sobre un enlace
 * que el controlador ya da por muerto **no genera ningun evento**, asi que
 * `onDisconnect` no llega nunca, `bt_conectado` se queda en true... y como el
 * nodo no se anuncia mientras cree tener un cliente, **desaparece para siempre
 * de los escaneos**. Ni reiniciandolo se arreglaba: el estado decia "movil
 * conectado" desde el primer segundo.
 * La version de SPP hacia bien esto: pedia la desconexion Y limpiaba a mano.
 * Se recupera esa disciplina, y ademas se vuelve a anunciar aqui mismo — no se
 * delega en nadie. */
static void suelta_ble(const char *motivo)
{
    log_usb(motivo);
    if (ble_srv && ble_conn != 0xFFFF) ble_srv->disconnect(ble_conn);
    ble_conn      = 0xFFFF;
    ble_mtu       = 23;
    ble_suscrito  = false;
    bt_conectado  = false;
    bt_autorizado = false;
    bt_hablo      = false;
    bt_ultimo     = millis();
    cierra_tubo(T_BT);
    if (bt_activo) NimBLEDevice::startAdvertising();
    redibujar = true;
}

static void suelta_bt_fantasma()
{
    /* PRIMERO, LA VERDAD DE NIMBLE, NO NUESTRA BANDERA.
       Si la pila dice que no hay nadie conectado y nosotros creemos que si, es
       que se perdio un `onDisconnect`. Sin esta comprobacion el nodo se queda
       invisible sin que nada lo explique; con ella se arregla solo en la
       siguiente vuelta del bucle. Es barato y cubre TODOS los caminos por los
       que se puede perder ese evento, conocidos y por conocer. */
    if (bt_conectado && ble_srv && ble_srv->getConnectedCount() == 0) {
        suelta_ble("BLE: nadie conectado de verdad; se corrige y se anuncia");
        return;
    }
    if (!bt_conectado) return;
    if (millis() - bt_ultimo < (bt_hablo ? BT_MUDO_MS : BT_MUDO_NUEVO_MS)) return;
    suelta_ble(bt_hablo ? "cliente BLE callado 45 s: se le suelta"
                        : "cliente BLE mudo: se le suelta");
}

static void resuelve_conexion_bt()
{
    if (bt_cerrado) {
        bt_cerrado = false;
        cierra_tubo(T_BT);
        redibujar = true;
        /* Ahora que la sesion ya no existe, se puede tocar la NVS sin romper
           nada: se apunta el movil para que la proxima vez entre directo. */
        if (hay_que_apuntar) {
            hay_que_apuntar = false;
            apunta_conocido(bt_por_apuntar);
            log_usb("movil apuntado como conocido");
        }
    }
    if (!bt_nuevo) return;
    bt_nuevo = false;
    bt_ultimo = millis();
    redibujar = true;
    /* Un cliente a la vez: mientras haya uno dentro, el nodo no se anuncia.
       Ver la nota de `CbServidorBLE`. */
    NimBLEDevice::stopAdvertising();
    // El Bluetooth es un tubo mas: se abre aqui y se cierra al desconectar.
    tubos[T_BT].clase = TUBO_BT;
    tubos[T_BT].n = 0; tubos[T_BT].dentro = false; tubos[T_BT].escape = false;
    tubos[T_BT].t_ultimo = millis();
    if (!pedir_codigo || es_conocido(bt_par)) {
        bt_autorizado = true;              // ya paso por aqui: entra directo
        tubos[T_BT].autorizado = true;
        log_txt(pedir_codigo ? "movil conocido: autorizado"
                             : "conectado (sin codigo: esta desactivado)");
    } else {
        bt_codigo = esp_random() % 1000000;   // aleatorio por hardware
        despierta_pantalla();
        char m[64];
        snprintf(m, sizeof m, "movil nuevo: codigo de acceso %06u",
                 (unsigned)bt_codigo);
        log_usb(m);                    // por cable, NUNCA por el Bluetooth
        tubos[T_BT].autorizado = false;
        kiss_a(tubos[T_BT], EV_EMPAREJA, (const uint8_t *)"codigo", 6);
    }
}

/* SEGURIDAD: DE MOMENTO, LA PUERTA SIGUE EN NUESTRO PROTOCOLO.
 *
 * Es decir: el codigo de 6 cifras que sale en la PANTALLA del nodo y que hay
 * que teclear en la app (`bt_codigo`, `CMD_CONFIRMA`). Sin el, lo unico que se
 * atiende es el propio codigo; todo lo demas se tira. Eso no cambia con BLE.
 *
 * Lo que SI cambia es que ahora hay un sitio mejor donde ponerla: el passkey
 * del propio BLE (`NimBLEDevice::setSecurityPasskey` + `setSecurityAuth`), que
 * es lo que hace Meshtastic. Con eso quien no sepa el numero **no llega a abrir
 * el GATT**, en vez de abrirlo y que le ignoremos.
 *
 * NO SE ACTIVA TODAVIA, Y A PROPOSITO. Con SPP ya se intento exigir seguridad
 * antes de comprobar que el otro extremo la entendia, y el resultado fue
 * dejar fuera a todo el mundo, el dueño incluido: Android decia "emparejando" y
 * soltaba la conexion sin llegar a pedir nada. La leccion de aquello es el
 * orden: **primero que conecte, despues se cierra la puerta**, comprobando cada
 * paso en el banco. El emparejamiento BLE se enchufa cuando lo de abajo este
 * validado con las dos placas y con los dos moviles del usuario.
 *
 * (El PIN de Bluetooth Classic —`setPin()`— era ademas papel mojado: con SSP
 * activado el emparejamiento va por "Just Works" o por comparacion de numeros y
 * el PIN de toda la vida ni se usa. Ese codigo se ha ido con el SPP.) */

static void visible_bt();

static void arranca_bt()
{
    /* Nombre del cacharro. Lleva el indicativo porque con varias placas encima
       de la mesa hay que saber cual es cual en la lista del movil.
       ⚠️ En BLE el nombre viaja en el ANUNCIO, que solo tiene 31 bytes: con el
       UUID del servicio (16 B + cabecera) y las banderas, para el nombre
       quedan pocos. Por eso el nombre completo va en la respuesta al escaneo
       (`setScanResponse(true)`), que da otros 31 bytes. Sin eso, un nombre
       largo hace que `start()` falle con "data too long" y la placa NO SE
       ANUNCIA — sin dar ningun error visible desde fuera, que es el peor
       modo de fallo posible. */
    if (nombre_nodo[0] == 0) {
        uint8_t m[6];
        if (esp_read_mac(m, ESP_MAC_BT) == ESP_OK)
            snprintf(nombre_nodo, sizeof nombre_nodo, "nodo-%02X%02X", m[4], m[5]);
        else
            snprintf(nombre_nodo, sizeof nombre_nodo, "nodo");
    }
    snprintf(nombre_bt, sizeof nombre_bt, "PTTLoRa-%s", nombre_nodo);

    if (bt_modo == BT_APAGADO) {
        /* Ni se arranca la pila. En una placa con la antena de BT/WiFi rota
           —que es el caso de la celda del tejado— encenderlo solo gasta
           memoria y corriente para que no lo vea nadie. */
        bt_ok = false;
        bt_activo = false;
        snprintf(nombre_bt, sizeof nombre_bt, "off");
        log_usb("BLE: apagado por ajuste (bt=0)");
        return;
    }

    uint32_t antes = ESP.getFreeHeap();
    NimBLEDevice::init(nombre_bt);
    /* Potencia del BLE al maximo (+9 dBm). No tiene nada que ver con la
       potencia de LoRa; es el enlace con el movil, y en un bolsillo o con el
       nodo dentro de una caja metalica se agradece. */
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    /* MTU: se PIDE 517 (el maximo del estandar). Quien lo negocia de verdad es
       el movil y puede quedarse en 23; `ble_escribe` trocea a lo que salga. */
    NimBLEDevice::setMTU(517);

    ble_srv = NimBLEDevice::createServer();
    ble_srv->setCallbacks(&cb_servidor_ble);

    NimBLEService *serv = ble_srv->createService(UUID_NUS_SERV);
    /* TX: el nodo notifica. NOTIFY y no INDICATE: indicate espera confirmacion
       de cada trozo y con voz en tiempo real eso es exactamente lo que no
       queremos — mas vale perder un lote que retrasar los siguientes. */
    ble_tx = serv->createCharacteristic(UUID_NUS_TX, NIMBLE_PROPERTY::NOTIFY);
    ble_tx->setCallbacks(&cb_tx_ble);
    /* RX: el movil escribe SIN respuesta (write-no-response). Es la mitad de
       viajes por radio y en un flujo de audio se nota; el orden lo garantiza el
       propio enlace BLE. */
    NimBLECharacteristic *rx =
        serv->createCharacteristic(UUID_NUS_RX,
                                   NIMBLE_PROPERTY::WRITE |
                                   NIMBLE_PROPERTY::WRITE_NR);
    rx->setCallbacks(&cb_rx_ble);
    serv->start();

    NimBLEAdvertising *ad = NimBLEDevice::getAdvertising();
    /* Anunciar el UUID del servicio permite a la app FILTRAR el escaneo por el,
       en vez de listar todo lo que hay alrededor y adivinar por el nombre. */
    ad->addServiceUUID(UUID_NUS_SERV);
    ad->setScanResponse(true);
    ad->setName(nombre_bt);

    bt_ok     = true;
    bt_activo = true;

    char aviso[144];
    snprintf(aviso, sizeof aviso,
             "BLE: servicio NUS en marcha como \"%s\", gasto %u B, quedan %u",
             nombre_bt, (unsigned)(antes - ESP.getFreeHeap()),
             (unsigned)ESP.getFreeHeap());
    log_usb(aviso);

    strncpy(bt_mac, NimBLEDevice::getAddress().toString().c_str(), sizeof bt_mac - 1);
    bt_mac[sizeof bt_mac - 1] = 0;
    /* NimBLE devuelve la MAC en minusculas y el resto del sistema (la app, las
       notas, el `estado`) la lleva en mayusculas desde siempre. Se iguala aqui
       para que no parezcan dos placas distintas. */
    for (char *c = bt_mac; *c; c++) if (*c >= 'a' && *c <= 'f') *c -= 32;

    visible_bt();
}

/* ANUNCIARSE. Un nodo que no se anuncia es indistinguible de uno apagado.
 *
 * Se guarda el resultado en `visible=` del estado y se reafirma cada minuto
 * mientras no haya nadie conectado. Viene de una cicatriz de la epoca del SPP:
 * `begin()` devolvia 1, el servidor arrancaba... y la placa no aparecia en
 * ningun escaneo, sin que nada lo dijera. `start()` es idempotente. */
static void visible_bt()
{
    if (!bt_ok || bt_conectado) return;
    bool ok = NimBLEDevice::startAdvertising();
    bt_visible = ok ? 0 : -1;
    if (!ok) log_usb("BLE: no se pudo empezar a anunciar");
}

/* El nombre del Bluetooth se fija UNA VEZ, al arrancar.
   Con SPP esto hacia `end()` + `begin()` para rebautizar en caliente y era
   fragil: la pila no siempre volvia a levantar y la placa se quedaba sin
   anunciarse (sintoma: de dos placas configuradas solo aparecia una al buscar
   desde el movil). En BLE se podria hacer en caliente sin tanto riesgo, pero se
   mantiene el mismo trato —guardar en NVS y aplicar al reiniciar— porque asi lo
   que el usuario ve en la lista del movil coincide siempre con lo que hay en la
   NVS, sin estados intermedios. */
static void renombra_bt()
{
    kiss_manda(EV_LOG, (const uint8_t *)"indicativo guardado; el nombre "
               "Bluetooth cambia al reiniciar", 61);
}

/* En un nodo plantado en un cerro no hay movil: anunciarse solo gasta. Se deja
   de anunciar a los 5 min SI nadie se ha conectado, asi que siempre queda una
   ventana tras cada arranque para configurarlo.
   ⚠️ Solo se para el ANUNCIO, no se desmonta NimBLE. Con SPP habia que llamar
   a `end()` para recuperar heap, y volver a `begin()` era fragil hasta el punto
   de dejar placas sin anunciarse. Aqui no hace falta: el anuncio parado ya no
   gasta radio, la pila entera son unos pocos kB, y asi el camino de vuelta
   (`visible_bt()`) es una sola llamada que no puede fallar a medias. */
static void apaga_bt_si_toca()
{
    if (!bt_activo || bt_conectado) return;
    if (bt_modo != BT_VENTANA) return;
    if (millis() < VENTANA_BT_MS) return;
    NimBLEDevice::stopAdvertising();
    bt_activo = false;
    redibujar = true;
    log_txt("BLE: se deja de anunciar (repetidor fijo). Reinicia el nodo para "
            "reconfigurarlo.");
}

// -------------------------------------------------------- salvaguardas ----
/* El watchdog del ESP32 solo caza un cuelgue de CPU, y ese NO es el fallo que
 * de verdad nos ha pasado: este nodo se muere con el bucle corriendo tan feliz
 * y la radio sorda. Por eso hay tres capas:
 *   1. Watchdog de tarea: si el bucle se para, reinicio.
 *   2. Rearmado de la recepcion tras un rato largo sin oir nada.
 *   3. Reinicio preventivo diario, y SOLO en el nodo de cerro: en uno que
 *      llevas encima seria una groseria cortarte a media conversacion.
 * El reinicio nunca cae con el canal ocupado ni transmitiendo. */
static void salvaguardas()
{
    esp_task_wdt_reset();

    if (millis() - t_ultimo_rx > RX_MUDO_MS) {
        t_ultimo_rx = millis();
        st_rx = armar_rx();
        log_txt("silencio largo: recepcion rearmada");
    }

    if (perfil == PERFIL_FIJO && millis() > REINICIO_MS
        && !transmitiendo && !canal_ocupado && !bt_conectado && cola_n == 0) {
        log_txt("reinicio preventivo de las 24 h");
        delay(200);
        ESP.restart();
    }
}

// --------------------------------------------------------------- wifi/ota --
/* El WiFi existe SOLO para poder actualizar el firmware sin bajar la placa del
 * sitio donde este. Nunca esta en el camino de la voz, y si no hay credenciales
 * o no hay punto de acceso, el nodo se comporta exactamente igual.
 * OJO: WiFi y Bluetooth clasico comparten la radio de 2,4 GHz y tenerlos a la
 * vez degrada los dos. En el nodo de cerro da igual porque alli el BT se apaga
 * solo; en un nodo de mano, mejor no poner credenciales. */
/* Servidores TCP. Dos puertos, y no uno con negociacion, porque por ellos
   viajan dos protocolos distintos: ordenes de anfitrion en uno y tramas del
   aire en el otro. Distinguirlos por el contenido seria adivinar. */
static WiFiServer srv_app(PUERTO_APP);
static WiFiServer srv_enlace(PUERTO_ENLACE);
/* Descubrimiento: el nodo contesta a quien pregunte por difusion. Asi la app
   no necesita que nadie se aprenda una IP que ademas cambia con el DHCP. */
static WiFiUDP udp_busca;
static bool servidores_en_pie = false;

static void arranca_wifi()
{
    if (red_modo == RED_OFF) return;

    if (red_modo == RED_AP) {
        /* El nodo levanta SU PROPIA red. Este es el caso de la excursion: no
           hace falta router, ni Internet, ni cobertura de nada — el grupo se
           cuelga del nodo que va en la mochila.
           Cuesta unos 100-150 mA frente a los ~40 del Bluetooth, asi que no se
           enciende solo: es una decision del usuario. */
        String ss = wifi_ssid.length() ? wifi_ssid : String("PTTLoRa-") + mi_indicativo;
        WiFi.mode(WIFI_AP);
        // Canal 6 y tantas estaciones como sesiones admite el nodo (TCP_N):
        // que no sobre sitio en la red para quien luego no va a poder entrar.
        WiFi.softAP(ss.c_str(), wifi_clave.c_str(), 6, 0, TCP_N);
    } else {
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(true);
        wifi_cual = 0;
        wifi_probando = millis();
        WiFi.begin(wifi_ssid.c_str(), wifi_clave.c_str());
        // NO se espera a que conecte: si el punto de acceso no esta, el nodo
        // tiene cosas mejores que hacer que quedarse mirando.
    }
    wifi_activo = true;
}

/* Se levantan cuando hay red de verdad: en modo AP en cuanto arranca, y en modo
   cliente solo despues de coger IP. */
static void arranca_servidores()
{
    if (servidores_en_pie) return;
    srv_app.begin();
    srv_app.setNoDelay(true);          // la voz no espera al algoritmo de Nagle
    srv_enlace.begin();
    srv_enlace.setNoDelay(true);
    udp_busca.begin(PUERTO_BUSCA);
    servidores_en_pie = true;
    redibujar = true;
}

/* Contesta a quien busque nodos en la red.
   La respuesta lleva lo justo para elegir sin conectarse: quien es, que version
   tiene, cuanta gente hay ya colgada y si el canal esta libre. */
static void atiende_busqueda()
{
    /* Solo cuando el socket existe. Sin esto, `parsePacket()` sobre un socket
       sin abrir suelta un error por el puerto serie en CADA vuelta del bucle
       ("could not receive data: 9", que es EBADF) mientras el WiFi busca red:
       ruido constante que en un nodo remoto tapa lo que de verdad importa. */
    if (!servidores_en_pie) return;
    int n = udp_busca.parsePacket();
    if (n <= 0) return;
    char p[32] = {0};
    int li = udp_busca.read(p, sizeof p - 1);
    if (li <= 0) return;
    if (strncmp(p, BUSCA_PREGUNTA, strlen(BUSCA_PREGUNTA)) != 0) return;
    char r[96];
    int k = snprintf(r, sizeof r, "%s%s v%s clientes=%u %s",
                     BUSCA_RESPUESTA, mi_indicativo, VERSION,
                     (unsigned)cuenta_clientes(),
                     canal_ocupado ? "ocupado" : "libre");
    udp_busca.beginPacket(udp_busca.remoteIP(), udp_busca.remotePort());
    udp_busca.write((const uint8_t *)r, k);
    udp_busca.endPacket();
}

/* Acepta clientes nuevos y coloca cada uno en un tubo libre. */
static void atiende_clientes()
{
    if (!servidores_en_pie) return;

    WiFiClient c = srv_app.available();
    if (c) {
        int libre = -1;
        for (int i = T_TCP0; i < T_ENL0; i++)
            if (tubos[i].clase == TUBO_LIBRE) { libre = i; break; }
        if (libre < 0) {
            // Sin sitio. Se le dice, en vez de dejarle un socket mudo.
            c.write("\xC0\x8F" "nodo lleno" "\xC0", 14);
            c.stop();
        } else {
            Tubo &t = tubos[libre];
            t.clase = TUBO_TCP;
            t.cli = c;
            t.cli.setNoDelay(true);
            t.buf = buf_tcp[libre - T_TCP0];
            t.cap = KISS_MAX_TCP;
            t.n = 0; t.dentro = false; t.escape = false;
            t.indicativo[0] = 0; t.src = 0;
            t.t_ultimo = millis();
            /* La puerta, igual que en Bluetooth pero con un matiz: si el nodo
               es SU PROPIO punto de acceso, la clave WPA2 ya es la puerta —
               quien esta dentro de la red la sabia. En cambio si el nodo esta
               colgado de una red ajena, cualquiera de esa red llega hasta aqui,
               y ahi si se pide el codigo que sale en la pantalla. */
            /* EL CODIGO ES PARA EL BLUETOOTH, NO PARA LA RED.
               Por Bluetooth entra cualquiera que pase por la calle: ahi el
               codigo es la unica puerta que hay. Por WiFi ya hay una — la
               clave de la red, que el nodo no puede volver a pedir sin
               estorbar a todo lo que le habla por ahi (herramientas, vigilante
               del repetidor, actualizaciones). Pedirlo tambien aqui convertiria
               cada script en un tramite manual sin ganar seguridad real:
               quien esta dentro de la red ya paso una puerta. */
            t.autorizado = true;
            if (!t.autorizado) {
                bt_codigo = random(100000, 1000000);
                char m[64];
                snprintf(m, sizeof m, "codigo para el cliente WiFi: %06u",
                         (unsigned)bt_codigo);
                log_usb(m);
                despierta_pantalla();
                kiss_a(t, EV_EMPAREJA, (const uint8_t *)"codigo", 6);
            }
            redibujar = true;
            manda_estado();
        }
    }

    // Enlaces ENTRANTES: otros nodos que se conectan a nosotros. Varios, que es
    // lo que permite que un nodo con IP alcanzable sea el punto de reunion de
    // toda la red.
    WiFiClient e = srv_enlace.available();
    if (e) {
        int libre = -1;
        for (int i = T_ENL0; i < TUBOS_N; i++)
            if (tubos[i].clase == TUBO_LIBRE) { libre = i; break; }
        if (libre < 0) { e.stop(); }
        else {
            Tubo &t = tubos[libre];
            t.clase = TUBO_ENLACE;
            t.cli = e;
            t.cli.setNoDelay(true);
            t.buf = buf_tcp[TCP_N + (libre - T_ENL0)];
            t.cap = KISS_MAX_TCP;
            t.n = 0; t.dentro = false; t.escape = false;
            t.autorizado = true;        // por el enlace no entran ordenes
            t.t_ultimo = millis();
            log_txt("enlace entrante conectado");
            redibujar = true;
        }
    }
}

/* Enlace SALIENTE. Se reintenta cada 15 s: al otro lado hay otro nodo que puede
   estar reiniciandose, o una linea que va y viene. Un enlace que se cae y no
   vuelve solo no sirve para unir dos valles. */
#define ENLACE_REINTENTO_MS 15000UL
/* Mantiene en pie el canal de mando saliente. Ver la nota de `mando_host`. */
static void atiende_mando()
{
    if (mando_host.length() == 0 || mando_puerto == 0) return;
    if (red_modo != RED_CLIENTE || WiFi.status() != WL_CONNECTED) return;
    if (transmitiendo || canal_ocupado) return;
    // ¿Sigue en pie el de antes?
    if (mando_tubo >= 0) {
        if (tubos[mando_tubo].clase == TUBO_TCP && tubos[mando_tubo].cli.connected())
            return;
        mando_tubo = -1;
    }
    if (t_mando && millis() - t_mando < ENLACE_REINTENTO_MS) return;
    t_mando = millis();

    /* Se coge la ULTIMA ranura de cliente: si el nodo esta lleno de gente por
       WiFi, el mando es lo ultimo que debe quitarle el sitio a nadie... pero
       tampoco puede quedarse sin ninguna, o el nodo remoto se vuelve
       inalcanzable justo cuando mas se le necesita. Por eso se reserva esta. */
    int libre = -1;
    for (int i = T_ENL0 - 1; i >= T_TCP0; i--)
        if (tubos[i].clase == TUBO_LIBRE) { libre = i; break; }
    if (libre < 0) return;

    Tubo &t = tubos[libre];
    if (!t.cli.connect(mando_host.c_str(), mando_puerto, 1000)) {
        t.cli.stop();
        return;
    }
    t.clase = TUBO_TCP;
    t.cli.setNoDelay(true);
    t.buf = buf_tcp[libre - T_TCP0];
    t.cap = KISS_MAX_TCP;
    t.n = 0; t.dentro = false; t.escape = false;
    /* Autorizado de entrada: al otro lado esta nuestro relevo, no un
       desconocido que pasaba por la calle. Pedirle un codigo que sale por una
       pantalla a la que nadie puede mirar seria una puerta sin llave. */
    t.autorizado = true;
    t.t_ultimo = millis();
    mando_tubo = libre;
    char m[80];
    snprintf(m, sizeof m, "canal de mando abierto contra %s:%u",
             mando_host.c_str(), (unsigned)mando_puerto);
    log_txt(m);
    redibujar = true;
}

static void atiende_enlace()
{
    if (red_modo != RED_CLIENTE || WiFi.status() != WL_CONNECTED) return;
    // Ni en mitad de una transmision ni con el canal ocupado: aunque el plazo
    // sea corto, un parón justo ahi se come lotes de voz de otros.
    if (transmitiendo || canal_ocupado) return;
    if (t_enlace && millis() - t_enlace < ENLACE_REINTENTO_MS) return;

    /* Los destinos son por PREFERENCIA, no simultaneos: el segundo es el
       respaldo del primero, para cuando el reflector de siempre se cae.
       Conectarse a los dos a la vez seria peor que mejor — se duplicaria el
       trafico (el descarte de duplicados lo taparia, pero gastando aire y
       linea) y, sobre todo, cada reflector arbitraria por su cuenta y podrian
       dejar pasar transmisiones distintas. */
    for (int k = 0; k < SALIENTES_N; k++) {
        if (enlace_host[k].length() == 0 || enlace_puerto[k] == 0) continue;
        // ¿Ya tiene ranura este destino? ¿Y hay alguno preferente en pie?
        bool puesto = false, hay_mejor = false;
        int libre = -1;
        for (int i = T_ENL0; i < TUBOS_N; i++) {
            if (tubos[i].clase == TUBO_ENLACE) {
                if (tubos[i].salida_k == k) puesto = true;
                if (tubos[i].salida_k >= 0 && tubos[i].salida_k < k) hay_mejor = true;
            }
            if (libre < 0 && tubos[i].clase == TUBO_LIBRE) libre = i;
        }
        if (hay_mejor) {
            /* El preferente ha vuelto: se suelta el respaldo, o nos quedariamos
               colgados de los dos para siempre. */
            if (puesto)
                for (int i = T_ENL0; i < TUBOS_N; i++)
                    if (tubos[i].clase == TUBO_ENLACE && tubos[i].salida_k == k) {
                        log_txt("respaldo soltado: el reflector principal ha vuelto");
                        cierra_tubo(i);
                    }
            continue;
        }
        if (puesto || libre < 0) continue;

        t_enlace = millis();       // un intento por vuelta: no encadenar esperas
        Tubo &t = tubos[libre];
        /* Con plazo corto y explicito: `connect()` espera 3 s por defecto, y un
           nodo parado 3 s en cada reintento pierde tramas de voz de todos. */
        if (!t.cli.connect(enlace_host[k].c_str(), enlace_puerto[k], 1000)) {
            t.cli.stop();
            return;
        }
        t.clase = TUBO_ENLACE;
        t.salida_k = k;
        t.cli.setNoDelay(true);
        t.buf = buf_tcp[TCP_N + (libre - T_ENL0)];
        t.cap = KISS_MAX_TCP;
        t.n = 0; t.dentro = false; t.escape = false;
        t.autorizado = true;
        t.t_ultimo = millis();
        char m[64];
        snprintf(m, sizeof m, "enlace establecido con %s", enlace_host[k].c_str());
        log_txt(m);
        redibujar = true;
        return;                    // uno por vuelta
    }
}

static void arranca_ota()
{
    if (wifi_clave.length() == 0) {
        log_txt("OTA no arranca: hace falta clave de WiFi (se usa como clave "
                "de actualizacion)");
        ota_lista = true;              // no reintentarlo cada vuelta
        return;
    }
    char host[40];
    snprintf(host, sizeof host, "pttlora-%s", mi_indicativo);
    ArduinoOTA.setHostname(host);
    // Con clave SIEMPRE y sin valor por defecto: una clave conocida en un
    // proyecto publico no es una clave.
    ArduinoOTA.setPassword(wifi_clave.c_str());
    ArduinoOTA.onStart([]() {
        // Una actualizacion tarda mas que el watchdog: hay que salirse de el o
        // la placa se reiniciaria a la mitad, dejandola a medio flashear.
        esp_task_wdt_delete(NULL);
        /* Y se duerme la radio. No es solo ahorro: sus interrupciones y el
           trafico SPI conviviendo con las escrituras en flash reiniciaban el
           nodo a media actualizacion (llegaba al 18%). Ademas no se puede
           hablar mientras te actualizas, asi que no se pierde nada. */
        radio.sleep();
        pantalla_modo_previo = pantalla_modo;
        log_txt("actualizacion OTA en marcha");
    });
    ArduinoOTA.onEnd([]() { log_txt("OTA completa, reiniciando"); });
    ArduinoOTA.onError([](ota_error_t e) {
        esp_task_wdt_add(NULL);
        char m[48];
        snprintf(m, sizeof m, "OTA fallo (%d)", (int)e);
        log_txt(m);
    });
    ArduinoOTA.begin();
    ota_lista = true;
}

static void atiende_wifi()
{
    if (!wifi_activo) return;
    if (red_modo == RED_AP) {
        arranca_servidores();
    } else if (WiFi.status() == WL_CONNECTED) {
        arranca_servidores();
        // La OTA por WiFi solo tiene sentido colgado de una red de verdad.
        if (!ota_lista) arranca_ota();
    } else if (wifi_ssid2.length() && millis() - wifi_probando > 20000UL) {
        /* Veinte segundos por red y a por la otra. Ni tan corto que corte un
           enganche a medias, ni tan largo que un nodo sin Bluetooth se pase
           minutos incomunicado en el unico sitio donde no puedes bajarlo. */
        wifi_cual = wifi_cual ? 0 : 1;
        wifi_probando = millis();
        const String &ss = wifi_cual ? wifi_ssid2  : wifi_ssid;
        const String &cl = wifi_cual ? wifi_clave2 : wifi_clave;
        WiFi.disconnect();
        WiFi.begin(ss.c_str(), cl.c_str());
        char m[80];
        snprintf(m, sizeof m, "WiFi: probando la red %s (%s)",
                 wifi_cual ? "segunda" : "primera", ss.c_str());
        log_usb(m);
    }
    if (ota_lista) ArduinoOTA.handle();
    atiende_clientes();
    atiende_busqueda();
    atiende_enlace();
    atiende_mando();
}

// ------------------------------------------------------------------ setup --
void setup()
{
    /* 1 kB de buffer de salida: la linea de estado no cabe en los 256 de serie
       y se perdia a cachos (ver kiss_a). Cuesta RAM, que sobra. */
    Serial.setTxBufferSize(1024);
    Serial.begin(115200);

    /* El cable es siempre el tubo 0 y no se cierra nunca: quien tiene el cable
       tiene el nodo en la mano, asi que entra ya autorizado. El Bluetooth es el
       1 y se abre cuando alguien se conecta. */
    tubos[T_USB].clase = TUBO_USB;
    tubos[T_USB].autorizado = true;
    tubos[T_USB].buf = buf_usb;
    tubos[T_USB].cap = KISS_MAX;
    tubos[T_BT].buf = buf_bt;
    tubos[T_BT].cap = KISS_MAX;

    pinMode(P_LED, OUTPUT);
    digitalWrite(P_LED, LOW);
    pinMode(P_BOTON, INPUT_PULLUP);
    analogReadResolution(12);
    carga_ajustes();
    randomSeed(esp_random());

    // NO TOCAR GPIO16 EN ESTA PLACA. El pins_arduino.h del variant lo declara
    // como OLED_RST, pero en la T3 v1.6.1 real manejarlo cuelga el sistema:
    // arranque en bucle con TG1WDT_SYS_RESET, siempre en ese punto exacto.
    // Tampoco hace falta: el SSD1306 conserva la imagen del firmware anterior
    // (tiene su propia memoria y la refresca solo), pero `begin()` lo
    // reinicializa entero por I2C y `clearDisplay()` la borra igual.
    Wire.begin(P_SDA, P_SCL, 400000);

#ifdef PLACA_TBEAM
    // TRAMPA DE LA T-BEAM: el SX1278 y el GPS no cuelgan del 3V3 general sino
    // de los raíles del AXP2101. Si no se encienden aqui, `radio.begin()` falla
    // y parece que la placa esta rota cuando lo que pasa es que la radio ni
    // tiene corriente.
    hay_pmu = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, P_SDA, P_SCL);
    if (hay_pmu) {
        pmu.setALDO2Voltage(3300); pmu.enableALDO2();   // radio LoRa
        pmu.setALDO3Voltage(3300); pmu.enableALDO3();   // GPS
        delay(100);
    }
    /* La UART del GPS se abre SIEMPRE, haya PMU o no: si no hay corriente en el
       rail simplemente no llegara nada, y eso se ve en el estado (`gps=sin
       señal`) en vez de quedar como un misterio. */
    Serial1.begin(GPS_BAUD, SERIAL_8N1, P_GPS_RX, P_GPS_TX);
#endif

    hay_oled = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    if (hay_oled) {
        oled.clearDisplay();
        oled.display();
    }

    SPI.begin(P_SCK, P_MISO, P_MOSI, P_CS);
    int st = radio.begin(frecuencia, ancho, sf, cr, SYNC_WORD, potencia, 8);
    if (st != RADIOLIB_ERR_NONE) {
        while (true) {                       // sin radio no hay nada que hacer
            char s[48];
            snprintf(s, sizeof s, "radio NO arranca: %d", st);
            log_txt(s);
            digitalWrite(P_LED, !digitalRead(P_LED));
            delay(1000);
        }
    }
    // Watchdog: si el bucle deja de dar señales, la placa se reinicia sola.
    // `true` = entrar en panico (y por tanto reiniciar), no solo avisar.
    esp_task_wdt_init(WDT_S, true);
    esp_task_wdt_add(NULL);

    radio.setCRC(true);
    radio.setCurrentLimit(140);
    radio.setPacketReceivedAction(al_recibir);
    st_rx = armar_rx();
    arranca_bt();
    arranca_wifi();
    // Que la primera baliza salga pronto, no dentro de HOLA_MS. El retardo
    // aleatorio es para que varias placas encendidas a la vez no se pisen.
    t_ultima_hola = millis() - HOLA_MS + 3000 + random(4000);
    log_txt("PTT LoRa v1 en marcha");
    pinta();
    manda_estado();
}

void loop()
{
    if (hay_paquete) {
        hay_paquete = false;
        uint8_t b[CAB_LEN + MAX_PAYLOAD];
        int len = radio.getPacketLength();
        if (len > 0 && len <= (int)sizeof b) {
            int st = radio.readData(b, len);
            if (st == RADIOLIB_ERR_NONE)
                procesar(b, len, radio.getRSSI(), radio.getSNR());
            else
                n_malas++;
        }
        st_rx = armar_rx();
    }

    leer_serie();
#ifdef PLACA_TBEAM
    gps_atiende();
#endif
    purgar_cola();

    // Aviso de canal ocupado: la app tiene que poder bloquear el PTT mientras
    // otro habla, que es la unica disciplina que hay en un canal simplex.
    bool ocupado = (millis() - t_ultimo_rx) < OCUPADO_MS;
    if (ocupado != canal_ocupado) {
        canal_ocupado = ocupado;
        redibujar = true;
        uint8_t v = ocupado ? 1 : 0;
        kiss_manda(EV_CANAL, &v, 1);
    }

    salvaguardas();
    vigila_ptt();
    resuelve_conexion_bt();
    /* Mientras haya una sesion esperando codigo, se repite por el CABLE cada
       10 s. Una pantalla puede estar en otra habitacion, apagada o no ser la
       del nodo al que se esta conectando el movil — y entonces el usuario se
       queda tecleando un numero que no es, sin forma de saberlo. */
    {
        static uint32_t t_rec = 0;
        if (bt_conectado && !bt_autorizado && millis() - t_rec > 10000UL) {
            t_rec = millis();
            char m[64];
            snprintf(m, sizeof m, "esperando codigo: es %06u", (unsigned)bt_codigo);
            log_usb(m);
        }
    }
    suelta_bt_fantasma();
    /* Se reafirma la visibilidad cada minuto mientras no haya nadie conectado.
       Un nodo que deja de anunciarse es indistinguible de uno apagado, y no hay
       forma de enterarse desde fuera: ver `visible_bt()`. */
    {
        static uint32_t t_visible = 0;
        /* Solo si el Bluetooth sigue encendido: en PERFIL_FIJO se apaga a los
           5 min y entonces esta llamada devuelve 259 (estado invalido) y llena
           el registro de un error que no lo es. */
        if (bt_activo && !bt_conectado && millis() - t_visible > 60000UL) {
            t_visible = millis();
            visible_bt();
        }
    }
    suelta_tcp_fantasma();
    caduca_ota();
    atiende_wifi();
    apaga_bt_si_toca();
    if (digitalRead(P_BOTON) == LOW) despierta_pantalla();
    gestiona_pantalla();

    // Refresco de pantalla: por evento, y en todo caso una vez por segundo para
    // que el estado del canal y la bateria no se queden congelados.
    if (redibujar || millis() - t_pantalla > 1000) pinta();

    // Baliza de indicativo. Es identificacion de estacion, no adorno: por eso
    // sale tambien cuando el nodo esta solo repitiendo, sin nadie hablando.
    // La baliza sale tambien al poco de arrancar (no solo cada HOLA_MS): un
    // nodo recien encendido tiene que anunciarse YA, o los demas lo tomaran por
    // inexistente y se callaran la repeticion que ese nodo necesita.
    if (millis() - t_ultima_hola > hola_espera && !transmitiendo && !ocupado)
        manda_hola();
}
