// protocolo.h — formato de trama de PTT LoRa y protocolo con el anfitrion.
//
// Un solo canal, una sola conversacion a la vez, todo en claro. Pensado para
// operar como estacion de radioaficionado: el indicativo viaja SIN cifrar y se
// manda al empezar cada transmision y en la baliza periodica.
//
// POR QUE NO RETICULUM AQUI: un paquete de Link de RNS cuesta 69 bytes de sobre.
// Con lotes de voz de 72 bytes eso es doblar el aire por cada salto, y ademas
// obliga a cifrar, que en banda de aficionado no procede. Reticulum se queda
// para unir nodos fijos por Internet, que es donde brilla.
//
// EL CODEC NO VIVE AQUI. El nodo solo mueve bytes: el movil codifica y el
// gateway decodifica. Asi se puede cambiar de modo de Codec2 en el campo sin
// reflashear nada, y el firmware no depende del codec.

#pragma once
#include <stdint.h>

#define PROTO_MAGIC   0xA1        // 0xA0 | version 1
// Version del firmware. Sale en el estado: es como se comprueba que una
// actualizacion por radio ha entrado de verdad.
#define VERSION       "1.51"

#define MAX_PAYLOAD   200         // holgado para un lote de 960 ms a 1200 bps
#define MAX_INDICATIVO 12

// ---- tipos de trama ----
enum : uint8_t {
    T_VOZ    = 1,   // lote de tramas de codec
    T_INICIO = 2,   // pulsacion de PTT: abre stream y da el indicativo
    T_FIN    = 3,   // soltar el PTT
    T_HOLA   = 4,   // baliza: indicativo, capacidades, bateria
    /* ⚠️ LA BALIZA QUE **NO SALE AL AIRE**. Va SOLO por el enlace de Internet,
       nunca por RF, y solo la mandan los nodos que tienen enlace — que son
       justo los que pueden permitirsela.
       Por que aparte y no metiendo mas bytes en la baliza de radio: **el aire
       es el unico recurso que no se puede ampliar**. Cada byte añadido a
       `T_HOLA` lo paga cada nodo, cada minuto, para siempre, incluidos los que
       no tienen Internet ni les hace falta. Aqui, en cambio, cabe todo lo que
       el censo de la red quiera saber sin costar un solo simbolo de RF.
       Se manda con **`saltos = 0`**, y eso no es un detalle: un nodo con
       firmware viejo no sabe que es el tipo 5, pero **no lo repetira igualmente
       porque no le quedan saltos**. El protocolo se protege solo. */
    T_INFORME = 5,
};

// ---- perfiles de nodo ----
//
// UN SOLO FIRMWARE hace todos los papeles: obligar a elegir binario, o a
// reflashear para cambiar de funcion, es pedirle al usuario un conocimiento que
// no tiene por que tener. El perfil se guarda en el nodo y se cambia desde la
// app.
enum : uint8_t {
    // Por defecto: hace de puente para el movil Y de repetidor, pero se calla
    // la repeticion si no hay a quien repetir (ver hay_a_quien_repetir).
    PERFIL_AUTO = 0,
    // Nodo plantado en un cerro sin movil: repite SIEMPRE, aunque solo oiga a
    // una estacion, porque puede haber gente que le oiga a el y el no.
    PERFIL_FIJO = 1,
    // Solo mi radio: no repite nunca. Para quien no quiera gastar bateria ni
    // aire en los demas.
    PERFIL_SOLO = 2,
};

// ---- gestion de la pantalla ----
// Con alimentacion de red da igual, pero con bateria o panel el SSD1306 son
// ~10-15 mA de media: sobre un nodo que consume ~2,5 Ah/dia, eso es un 12% del
// presupuesto gastado en algo que nadie esta mirando.
enum : uint8_t {
    PANTALLA_AUTO  = 0,   // se enciende con actividad y se apaga sola (por defecto)
    PANTALLA_FIJA  = 1,   // siempre encendida: nodo con alimentacion de red
    PANTALLA_OFF   = 2,   // siempre apagada: maximo ahorro
};

// ---- modos de codec (los del propio Codec2) ----
// El nodo NO interpreta esto, solo lo copia. Va en la trama para que dos
// estaciones con distinto ajuste se entiendan: manda lo que dice el emisor.
enum : uint8_t {
    C2_3200 = 0, C2_2400 = 1, C2_1600 = 2, C2_1300 = 3,
    C2_1200 = 4, C2_700C = 5,
};

// ---- cabecera comun: 8 bytes ----
//
//   0  magic|version
//   1  canal
//   2  tipo (4 bits altos) | saltos restantes (4 bits bajos)
//   3  src[0]   \
//   4  src[1]    > 24 bits: hash del indicativo. El indicativo entero solo
//   5  src[2]   /  viaja en INICIO y HOLA; en la voz seria un 12% de sobre por
//                  trama para repetir lo mismo 20 veces por segundo.
//   6  stream   nuevo por cada pulsacion de PTT (aleatorio)
//   7  seq      numero de lote dentro del stream
//
// El dedupe de la malla es (src, stream, seq, tipo): con eso un repetidor sabe
// que ya ha oido algo aunque le llegue por tres caminos distintos.
#define CAB_LEN 8

struct Cabecera {
    uint8_t  magic;
    uint8_t  canal;
    uint8_t  tipo;
    uint8_t  saltos;
    uint32_t src;        // solo 24 bits utiles
    uint8_t  stream;
    uint8_t  seq;
};

// Cuerpo segun tipo:
//   T_VOZ    : [modo][n_tramas][datos...]
//   T_INICIO : [modo][indicativo]\0[lat:4][lon:4]   (la cola, desde la v1.35)
//   T_FIN    : (vacio)
//   T_HOLA   : [flags][bateria %][indicativo]\0[nombre]\0[lat:4][lon:4]
//   T_INFORME: texto `clave=valor` separado por espacios. SOLO por el enlace.
//              Texto y no binario a proposito: no paga aire, asi que lo que
//              importa es poder añadir campos sin romper a quien ya lo lea —
//              el que no entienda una clave se la salta. El grafo va en
//              `vec=<src>:<rssi>:<papel>,...`, y ese `rssi` es el dato que
//              convierte una lista de vecinos en un mapa de sombras: saber que
//              A oye a X no dice si el enlace esta holgado o al limite.
//
// LA BALIZA CRECE POR EL FINAL, SIEMPRE DETRAS DE UN CERO. Es el mismo truco
// con el que entro el nombre en la v1.30 y ahora la posicion en la v1.34: quien
// no entienda el campo nuevo corta en el primer cero y lee lo de siempre.
//   v1.29 y anteriores  ->  leen el indicativo, y ya
//   v1.30 .. v1.33      ->  leen indicativo y nombre, y la cola les sobra
//   v1.34               ->  ademas, lat y lon
// El nombre puede ir vacio, pero SU CERO VIAJA IGUAL cuando hay posicion: los
// campos se cuentan por separadores, no por presencia.
//
// DONDE VA LA POSICION, Y POR QUE NO ES SIEMPRE EN LA BALIZA (v1.35):
//
//   * la de una CELDA (fija, tecleada) va en la BALIZA, ciclicamente. Es
//     infraestructura: tiene que estar en el mapa aunque no hable nadie en
//     todo el dia, porque es lo que se mira para decidir donde cae la sombra
//     y donde poner la siguiente.
//   * la de una PERSONA (viva, la que le pasa su movil) va SOLO EN EL INICIO
//     DE CADA TRANSMISION, detras del indicativo y tras un `\0`. No sale
//     ciclicamente.
//
// La diferencia no es tecnica, es de a quien describe cada dato. Una celda es
// un sitio; una persona no. Emitir cada minuto por donde anda alguien es un
// rastro que ademas nadie ha pedido, y ocupa aire para decir lo mismo. Yendo en
// el INICIO, la posicion sale cuando esa estacion **se identifica de todas
// formas**, que es exactamente cuando interesa saber desde donde habla — y no
// cuesta ni una trama de mas, solo 9 bytes en una que ya iba a salir.
//
// POSICION: lat y lon son enteros con signo de 4 bytes, little-endian, en
// DIEZMILLONESIMAS DE GRADO (1e-7). Cabe el planeta entero (±1.800.000.000) con
// resolucion de ~1 cm, que sobra, pero es el formato que usan todos —NMEA
// convertido, APRS, OSM— y evita el coma flotante en la trama.
// Coste medido a SF7/BW250: la baliza pasa de 36,0 a 41,1 ms. Veinte nodos
// balizando cada minuto son el 1,37 % del canal. Es gratis.
//
// ⚠️ Y LA POSICION SE LEE POR EL FINAL, NO PARTIENDO LA COLA POR CEROS. Son 8
// bytes BINARIOS y cualquiera puede valer cero: una longitud de 1,5 grados es
// 15.000.000 = C0 E1 E4 00, con el cero dentro. Quien parta toda la cola por
// `\0` —que es lo natural despues de haberlo hecho con el nombre— se encuentra
// la posicion troceada. Lo correcto: si viene el flag HOLA_POS, se apartan los
// 8 ultimos bytes y su separador, y SOLO ENTONCES se parte el texto que queda.
//
// flags de T_HOLA:
#define HOLA_REPETIDOR  0x01     // repite lo que oye
#define HOLA_PUENTE     0x02     // tiene anfitrion conectado (movil o gateway)
/* ES UNA CELDA: repetidor FIJO, o sea infraestructura.
 *
 * No es lo mismo que HOLA_REPETIDOR, y la diferencia es todo el diseño: aquel
 * lo pone cualquier nodo que repita, incluido un movil en el bolsillo de
 * alguien que se va a ir dentro de diez minutos. Este lo pone solo un nodo
 * puesto ahi a proposito, con su sitio, su antena y su corriente.
 *
 * Sirve para que un cliente sepa que **tiene cobertura de infraestructura** y
 * pueda callarse: donde hay celda no hace falta inundar, y inundar es
 * justamente lo que hace que la voz no llegue (medido: 49% de entrega
 * inundando, 100% con celda). Es el TMO/DMO de TETRA con un bit.
 *
 * Compatible hacia atras: los firmwares anteriores a la v1.27 leen este byte y
 * solo miran los bits 0x01 y 0x02, asi que este se lo saltan sin enterarse. */
#define HOLA_CELDA      0x04     // repetidor fijo = infraestructura
/* LLEVA POSICION AL FINAL. Ver el mapa de la baliza justo debajo.
 *
 * Es un flag y no "mirar si sobran bytes" porque las dos cosas que se quieren
 * saber son distintas: si el nodo SABE donde esta, y si esa posicion viene con
 * la baliza. Un nodo puede tener posicion y no publicarla (ver la nota de
 * privacidad de abajo), y entonces este bit va a cero y la cola no viaja. */
#define HOLA_POS        0x08     // detras del nombre van lat y lon
/* POR AQUI SE SALE A INTERNET. Cuesta **cero bytes de aire** —el byte de flags
   ya viajaba— y es de las pocas cosas de Internet que un vecino de RADIO
   necesita saber: por donde sale su zona al mundo. Eso lo tiene que saber un
   nodo, no un servidor. Todo lo demas del enlace va en `T_INFORME`, que no
   gasta aire. */
#define HOLA_ENLACE     0x10     // este nodo tiene enlace de Internet en pie

/* ---- de donde viene una trama, para el anfitrion ----
 *
 * El campo `rssi` que se le entrega al movil es una medida de radio SALVO dos
 * valores imposibles por el aire, que son marcas:
 *
 *   127 = vino por el ENLACE de Internet.
 *   126 = ECO LOCAL: otro cliente de ESTE MISMO nodo. Ni ha salido por la
 *         antena ni ha tocado Internet.
 *
 * Los dos eran 127 hasta la v1.47, y eso mentia de una forma que importa:
 * hablando con alguien colgado del mismo nodo, la app enseñaba 🌐 y apuntaba
 * la voz como "tapada por Internet" **sin que hubiera Internet de por medio**.
 * Y con las cuentas de radio e Internet es justamente con lo que se decide si
 * la cobertura esta bien o mal: un dato asi manda a mirar donde no es. */
#define RSSI_ENLACE     127
#define RSSI_LOCAL      126
                                 // Solo lo pone una CELDA: ver mas abajo donde
                                 // va la posicion de cada cual.

// ---------------------------------------------------------------- anfitrion --
//
// Con el movil o con el gateway se habla por USB serie (y mas adelante BLE) en
// tramas KISS, que es lo que usa media radio digital y evita inventar otro
// entramado. Byte 0 de cada trama KISS = orden o evento.

#define KISS_FEND   0xC0
#define KISS_FESC   0xDB
#define KISS_TFEND  0xDC
#define KISS_TFESC  0xDD

// anfitrion -> nodo
enum : uint8_t {
    CMD_INICIO = 0x01,   // [modo]                         abre stream (PTT abajo)
    CMD_VOZ    = 0x02,   // [modo][n][datos]               lote de voz
    CMD_FIN    = 0x03,   // -                              PTT arriba
    CMD_CONFIG = 0x04,   // [canal][saltos][potencia][perfil][indicativo...]
                         // Los dos ultimos son opcionales y en ese orden; el
                         // indicativo empieza donde acaban. Se distinguen sin
                         // ambiguedad porque un indicativo es ASCII imprimible
                         // y ni la potencia (2-17) ni el perfil (0-2) lo son.
    CMD_ESTADO = 0x05,   // -                              pide un EV_ESTADO
    CMD_CONFIRMA = 0x09, // [codigo de 6 cifras en ASCII] autoriza esta conexion
                         //
                         // POR QUE EN EL PROTOCOLO Y NO EN EL BLUETOOTH: se
                         // comprobo que `enableSSP()` NO impide una conexion
                         // RFCOMM sin emparejar — un cliente cualquiera abre el
                         // SPP y ya esta dentro. Asi que la puerta se cierra
                         // aqui: hasta que no llega el codigo correcto, el nodo
                         // ignora TODO lo demas. El codigo sale por la pantalla
                         // del nodo, nunca por el propio Bluetooth: si viajara
                         // por el mismo canal no serviria de nada.
    // --- actualizacion de firmware por Bluetooth ---
    // No necesita WiFi ni red de ninguna clase: el movil manda el firmware por
    // el mismo enlace por el que habla. Para un nodo que tienes a mano es el
    // camino natural; el de WiFi se queda para el nodo al que no puedes subir.
    CMD_OTA_INI = 0x0B,  // [tamaño:4][md5 en hex ASCII:32]
    CMD_OTA_DAT = 0x0C,  // [n_trozo:2][trozo del firmware]
                         // Lleva numero porque HACE FALTA reenviar: durante el
                         // borrado de un sector de flash (cada 4 kB) se pierden
                         // bytes de la cola del Bluetooth, la trama se corrompe
                         // y sin reenvio la actualizacion se muere ahi.
    CMD_OTA_FIN = 0x0D,  // -   cierra, verifica el MD5 y reinicia
    CMD_RADIO  = 0x0A,   // [kHz:4][bw_kHz:2][sf][cr][dBm]  ajustes de radio
                         // El nodo publica sus LIMITES en el estado (fmin/fmax/
                         // pmin/pmax): la app construye los rangos con lo que
                         // diga el hardware, no con lo que ella suponga.
    CMD_WIFI   = 0x08,   // [ssid]\0[clave]   credenciales; vacio = apagar WiFi.
                         // El WiFi NUNCA es imprescindible: si no hay
                         // credenciales o no hay punto de acceso, el nodo
                         // funciona igual. Se mantiene por compatibilidad con
                         // la app 0.5.0: equivale a CMD_RED en modo cliente.

    /* --- varios usuarios sobre un mismo nodo (WiFi) --- */
    CMD_IDENT  = 0x0E,   // [indicativo ASCII]
                         // Cada cliente declara EL SUYO. Es el cambio de fondo
                         // que pide el multiusuario: hasta ahora el nodo ponia
                         // su propio indicativo en el INICIO, lo que valia con
                         // un solo movil por Bluetooth pero seria emitir con
                         // indicativo ajeno en cuanto hay varios. Sin esta
                         // orden se sigue usando el del nodo, asi que los
                         // clientes viejos siguen funcionando.
    CMD_RED    = 0x0F,   // [modo][ssid]\0[clave]
                         // modo: RED_OFF / RED_CLIENTE / RED_AP.
                         // El punto de acceso propio es lo que permite dar
                         // servicio a un grupo SIN infraestructura ninguna: el
                         // nodo va en la mochila y los moviles se cuelgan de el.
    CMD_ENLACE = 0x10,   // [puerto:2][host ASCII]   vacio = soltar el enlace
                         // Une por Internet dos zonas de cobertura LoRa que no
                         // se oyen entre si.
    CMD_AIRE   = 0x11,   // [trama de radio COMPLETA, con su cabecera]
                         // Lo que viaja por un enlace entre nodos. Ojo: NO son
                         // ordenes, son tramas del aire tal cual. Por eso el
                         // enlace tiene su propio puerto: mezclarlo con el de
                         // los clientes obligaria a distinguir por contenido.
    CMD_RED2   = 0x12,   // [ssid]\0[clave]  segunda red WiFi (vacio = olvidar)
                         // Un nodo se configura donde estas y tiene que
                         // arrancar donde va: con dos redes guardadas la misma
                         // placa vale en los dos sitios sin tocarla.
    CMD_MANDO  = 0x13,   // [puerto:2][host ASCII]   vacio = soltar el mando
                         // Canal de mando SALIENTE: el nodo llama el. Es la
                         // unica forma de administrar un nodo en una red donde
                         // no se pueden abrir puertos entrantes — el caso de
                         // un sitio prestado. Por el va el protocolo entero,
                         // actualizacion de firmware incluida.
    CMD_AMPLI  = 0x14,   // [pin][previo_ms][cola_ms][preambulo]
                         // Amplificador externo. `pin` 0 = sin linea de PTT.
                         // El preambulo va aqui y no en CMD_RADIO porque su
                         // unico motivo de ser largo es dar tiempo a un
                         // amplificador que conmuta por deteccion de RF.
                         // OJO: 0x11 NO se puede usar, es CMD_AIRE (lo fue un
                         // rato por descuido: dos ordenes con el mismo codigo
                         // no dan error de compilacion, se tratan en switch
                         // distintos, y el fallo aparece mucho despues).
    /* [modo]  0=BT apagado siempre, 1=ventana de 5 min y se apaga, 2=siempre.
     *
     * ⚠️ ANTES ESTO IBA PEGADO AL PERFIL, y estaba mal. `PERFIL_FIJO` apagaba
     * el Bluetooth a los 5 minutos porque se supuso que un repetidor fijo vive
     * en un cerro y no tiene moviles cerca. Con la arquitectura de celda eso es
     * justo al reves: **la celda es el nodo al que mas te quieres poder
     * conectar**. Y a la vez hay celdas donde de verdad hay que apagarlo — la
     * del tejado tiene la antena de BT/WiFi rota, asi que encenderlo solo gasta.
     * Dos necesidades opuestas con el mismo perfil: por eso es ajuste propio. */
    CMD_BT     = 0x16,   // [modo]  ver arriba
    /* [origen][lat:4][lon:4] little-endian, en diezmillonesimas de grado.
     * Vacio = olvidar la posicion y dejar de publicarla.
     *
     * DOS ORIGENES, y la diferencia es la memoria flash. Un movil manda su GPS
     * cada pocos segundos: guardar eso en NVS se cargaria la flash en semanas
     * (100.000 ciclos de escritura por sector). Una celda, en cambio, tiene que
     * saber donde esta despues de un corte de luz, sin nadie delante.
     *   POS_VIVA (movil o GPS propio): en RAM, y CADUCA. Un nodo que lleva
     *     media hora sin noticias del GPS deja de publicar posicion en vez de
     *     seguir jurando que esta donde estuvo.
     *   POS_FIJA (se teclea una vez): en NVS, y no caduca nunca. Es lo que
     *     lleva una celda: no se mueve, y el mapa de la red la necesita. */
    /* Reiniciar el nodo. Sin datos.
     *
     * Hacia falta y no estaba, y se noto el 9-sep: guardar un WiFi nuevo no
     * tenia efecto hasta reiniciar, y **no habia forma de reiniciar** salvo ir
     * a la placa y quitarle la corriente. Para el nodo que llevas encima es una
     * molestia; para una celda en un tejado ajeno es no poder arreglarlo.
     *
     * Se ignora mientras se transmite o hay una actualizacion en curso: cortar
     * una OTA por la mitad no rompe nada (la placa sigue con el firmware que
     * tenia) pero obliga a repetirla entera, y son cinco minutos. */
    CMD_REINICIA = 0x18, // -
    CMD_POS    = 0x17,   // [origen][lat:4][lon:4]   posicion del nodo
                         // Vacio = olvidarla Y DEJAR DE PUBLICAR (tambien la
                         // del GPS propio); con datos, se vuelve a publicar.
    CMD_NOMBRE = 0x15,   // [nombre ASCII]  como se llama el CACHARRO en la
                         // lista del movil. Aparte del indicativo a proposito:
                         // el indicativo identifica la estacion por radio y
                         // cambia; el nombre identifica la placa y no.
};

/* Origen de la posicion, y CADA UNO SE EMITE DE UNA MANERA (v1.36).
 *
 * La regla no mira el perfil del nodo sino QUIEN SABE DONDE ESTA EL CACHARRO,
 * que es lo que de verdad distingue los casos:
 *
 *   POS_FIJA  tecleada una vez. Es una CELDA: un sitio. Va en la BALIZA,
 *             ciclicamente y para siempre, porque tiene que estar en el mapa
 *             aunque no hable nadie en todo el dia.
 *   POS_GPS   el aparato la sabe EL SOLO. Es un tracker, y se comporta como
 *             tal: BALIZA CICLICA, igual que un LoRa APRS. No depende de que
 *             haya alguien con un movil al lado, asi que un nodo plantado en un
 *             cerro se dibuja solo en el mapa sin teclear coordenadas.
 *   POS_VIVA  se la presta un MOVIL por CMD_POS. Eso no es la posicion de un
 *             cacharro, es la de una PERSONA: sale SOLO en el INICIO de cada
 *             transmision, cuando esa estacion se identifica de todas formas.
 *
 * Quien manda si hay varias: la tecleada gana siempre (es una decision), el GPS
 * propio gana al movil mientras tenga arreglo fresco (es el dato del propio
 * aparato y no depende de nadie), y el movil entra cuando no hay ninguna de las
 * otras dos. Sin esta precedencia el origen bailaria, y con el bailaria tambien
 * la forma de emitir — que es lo que no puede pasar.
 *
 * Y `pos_publica` lo apaga TODO, GPS incluido, porque si no el interruptor de
 * la app dejaria de gobernar un nodo con GPS propio. Se guarda en NVS: un nodo
 * desatendido tiene que seguir callado despues de un corte de luz. */
enum : uint8_t {
    POS_VIVA = 0,   // se la presta un movil: en RAM, caduca, solo al transmitir
    POS_FIJA = 1,   // tecleada: en NVS, no caduca, baliza ciclica
    POS_GPS  = 2,   // GPS del propio aparato: en RAM, caduca, baliza ciclica
};

// Modos de red del nodo (CMD_RED).
enum : uint8_t {
    RED_OFF     = 0,   // por defecto: el nodo no enciende la radio de 2,4 GHz
    RED_CLIENTE = 1,   // se cuelga de una red existente (casa, movil compartiendo)
    RED_AP      = 2,   // levanta SU PROPIA red: no hace falta nada mas
};

// Puertos TCP del nodo. Dos, y no uno con negociacion, porque por ellos viajan
// dos protocolos distintos: ordenes en uno y tramas de aire en el otro.
#define PUERTO_APP     4460
#define PUERTO_ENLACE  4461

/* Descubrimiento en la red local, por UDP.
 *
 * Sin esto hay que saberse la IP del nodo de memoria, que es justo lo que un
 * usuario no puede adivinar — y menos cuando el nodo la coge por DHCP y le
 * cambia. La app manda `PTTLORA?` a la direccion de difusion y los nodos
 * contestan con una linea de texto.
 *
 * POR QUE UN BROADCAST PROPIO Y NO mDNS: el descubrimiento de servicios del
 * sistema (NsdManager) es una loteria en los Android viejos, que es justo el
 * publico de esto. Un broadcast UDP se comporta igual en 4.4 que en 15 y son
 * treinta lineas en cada lado.
 */
#define PUERTO_BUSCA   4462
#define BUSCA_PREGUNTA "PTTLORA?"
#define BUSCA_RESPUESTA "PTTLORA "     // + indicativo, version, clientes, estado

// nodo -> anfitrion
enum : uint8_t {
    EV_INICIO = 0x81,   // [rssi][snr][src×3][stream][modo][indicativo...]
    EV_VOZ    = 0x82,   // [rssi][snr][src×3][stream][seq][modo][n][datos]
    EV_FIN    = 0x83,   // [src×3][stream]
    EV_HOLA   = 0x84,   // [rssi][snr][src×3][flags][bateria][indicativo...]
    EV_ESTADO = 0x85,   // texto: version, canal, indicativo, contadores
    EV_CANAL  = 0x86,   // [ocupado] el canal se ha puesto/dejado de estar ocupado
    EV_EMPAREJA = 0x87, // [codigo BCD de 6 cifras en texto] alguien quiere
                        // emparejarse: hay que comparar ese numero con el que
                        // sale en el movil y confirmar
    EV_OTA    = 0x88,   // [%] o texto: marcha de la actualizacion
    EV_PTT    = 0x89,   // [estado][indicativo del que habla]
                        // Arbitraje del microfono cuando hay varios usuarios
                        // colgados del mismo nodo: PTT_LIBRE / PTT_TUYO /
                        // PTT_DE_OTRO. Es distinto de EV_CANAL, que habla del
                        // aire: aqui puede estar el canal libre y aun asi no
                        // tocarte hablar porque otro del grupo tiene el PTT.
    EV_RED    = 0x8A,   // texto: modo de red, IP, clientes conectados, enlace
    EV_LOG    = 0x8F,   // texto suelto para depurar
};

// Estados de EV_PTT.
enum : uint8_t {
    PTT_LIBRE   = 0,
    PTT_TUYO    = 1,
    PTT_DE_OTRO = 2,
};

// Hash de indicativo a 24 bits (FNV-1a truncado). No es criptografia: solo
// distingue estaciones en las tramas de voz. Las colisiones se resuelven solas
// porque el indicativo de verdad llega en el INICIO de cada transmision.
static inline uint32_t hash_indicativo(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        char c = (*s >= 'a' && *s <= 'z') ? (*s - 32) : *s;   // sin mayusculas/minusculas
        h ^= (uint8_t)c;
        h *= 16777619u;
    }
    return h & 0xFFFFFF;
}
