/*
 * BANCO: la placa LoRa como PASARELA DE MANOS LIBRES (HFP-AG) de un
 * micro-altavoz Bluetooth. Primer ladrillo del transceptor autónomo.
 *
 * Qué se prueba, sin radio (se usa la placa que tiene el SX1278 muerto):
 *   1. Que el ESP32 encuentra, empareja y conecta el micro-altavoz (JBL de
 *      prueba; después el Abbree) haciendo de "teléfono".
 *   2. Que abre el canal de AUDIO (SCO, CVSD a 8 kHz — lo que come Codec2 sin
 *      remuestrear) y le llegan muestras del micro y le salen al altavoz.
 *   3. ECO POR CODEC2: lo que dices al micro se codifica en 1200, se guarda
 *      1,5 s y se descodifica de vuelta al altavoz. Si te oyes, funciona el
 *      camino entero: Bluetooth -> Codec2 -> Bluetooth.
 *   4. QUÉ MANDA CADA BOTÓN. En manos libres no hay "PTT": los botones llegan
 *      como órdenes AT (contestar, colgar, asistente de voz, volumen, o alguna
 *      propia del fabricante). Aquí se apuntan TODAS, que es lo que dirá cómo
 *      encajar el PTT del Abbree.
 *
 * Por la consola serie (115200), una orden por línea:
 *   buscar            10 s buscando equipos Bluetooth
 *   conecta AA:BB..   conectar con ese (el último se recuerda y se reconecta)
 *   audio / corta     abrir o cerrar el canal de audio
 *   eco / tono / nada qué suena en el altavoz
 *   estado            memoria, conexión, audio
 *
 * ⚠️ Esto necesita Arduino como componente de ESP-IDF: el core precompilado
 * trae el Bluetooth en papel de AURICULAR y sin canal SCO. Ver sdkconfig.defaults.
 */
#include <Arduino.h>
#include <Preferences.h>
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_ag_api.h"
#include "freertos/ringbuf.h"
#include "codec2.h"
/* LA PANTALLA. En un transceptor autonomo no hay movil al que preguntarle
   nada: la OLED es el UNICO instrumento. Dice quien habla y con cuanta señal,
   cuanto llevas transmitiendo y cuanto te queda de TOT, si el micro Bluetooth
   esta enganchado, la bateria y el canal.
   ⚠️ La fuente de Adafruit GFX es de 7 bits: en la pantalla, TEXTO SIN TILDES
   (una 'ñ' en UTF-8 son dos bytes y salen dos garabatos). */
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#define P_SDA 21
#define P_SCL 22
#define P_BAT 35        // divisor 2:1 hacia la bateria (LoRa32 v2.1)
#define P_BOTON 0       // el boton de fabrica ("PRG"): despierta la pantalla
#ifdef TRANSPORTE_LORA
/* TRANSCEPTOR DE VERDAD: la voz sale y entra por el SX1278, sin WiFi. Así el
   audio Bluetooth tiene la antena del ESP32 para él solo (con WiFi se perdía
   el 9,6 % del micro). Mismos parámetros y formato de trama que la red. */
#include <SPI.h>
#include <RadioLib.h>
static SX1278 radio = new Module(18, 26, 23, 33);          // CS, DIO0, RST, DIO1 (LoRa32 v2.1)
static volatile bool hay_paquete = false;
static ICACHE_RAM_ATTR void al_recibir() { hay_paquete = true; }
#else
#include <WiFi.h>
#include "esp_coexist.h"
#include "esp_wifi.h"
#if __has_include("secreto/wifi.h")
#include "secreto/wifi.h"          // WIFI_SSID / WIFI_CLAVE, fuera del repo
#else
#include "secreto-ejemplo/wifi.h"  // copia esto a secreto/wifi.h con tu red
#endif
#endif

/* RED: el walkie habla con el reflector de PTT LoRa como una celda más. */
static const char *REFLECTOR = "or.adan.ovh";
static const uint16_t REFLECTOR_PUERTO = 4461;
/* Con SSID propio: con el indicativo pelado la placa llevaría el MISMO src que
   el móvil del operador, y la app descarta lo que cree que es su propio eco. */
/* ⚠️ `NOCALL-5` a proposito, como en el resto del arbol publico: esto EMITE, y
   un indicativo de verdad por defecto haria salir al aire con el de otro. Pon
   el tuyo aqui antes de compilar. */
static const char *INDICATIVO = "NOCALL-5";
static const uint8_t CANAL = 1, SALTOS = 3, C2_1200 = 4, TRAMAS_LOTE = 12;
static const uint8_t FEND = 0xC0, FESC = 0xDB, TFEND = 0xDC, TFESC = 0xDD, CMD_AIRE = 0x11;
static const uint8_t T_VOZ = 1, T_INICIO = 2, T_FIN = 3;

static uint32_t hash_indicativo(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { char c = (*s >= 'a' && *s <= 'z') ? *s - 32 : *s; h ^= (uint8_t)c; h *= 16777619u; }
    return h & 0xFFFFFF;
}

extern "C" {
void *codec2_malloc(size_t t)             { return malloc(t); }
void *codec2_calloc(size_t n, size_t t)   { return calloc(n, t); }
void  codec2_free(void *p)                { free(p); }
}

/* ⚠️ Sin esto Arduino, al arrancar, LIBERA la memoria del Bluetooth
   (`initArduino` llama a esp_bt_controller_mem_release si btInUse() dice que
   no) y luego el controlador ya no se puede iniciar: ESP_ERR_INVALID_STATE. */
extern "C" bool btInUse() { return true; }   // en C: si no, no sustituye a la de Arduino

static Preferences prefs;
static esp_bd_addr_t par = {0};
static bool hay_par = false;
static volatile bool slc = false, audio_abierto = false;
/* EL PTT. En el JBL el botón "tel" manda AT+BVRA=1 / AT+BVRA=0 (asistente de
   voz): una pulsación abre, la siguiente cierra. Medido el 15-sep. */
static volatile bool ptt = false;
/* TOT: el corte por tiempo de transmision. Aqui no es un lujo — el PTT de un
   micro Bluetooth es un ESTADO que se queda puesto (una pulsacion abre, otra
   cierra), asi que un boton mal dado o el micro en el bolsillo dejarian la
   portadora en el aire hasta agotar la bateria. Mismo limite que la app. */
static const uint32_t TOT_MS = 180000;
static volatile uint32_t ptt_desde = 0;       // millis() del PTT abajo
static volatile bool tot_cortado = false;     // el TOT le quito el microfono
/* Lo que la pantalla necesita saber del trafico. */
static char nombre_par[24] = "";              // como se llama el micro-altavoz
static char hablante[16] = "";                // quien esta hablando ahora
static volatile int hablante_rssi = 0;
static volatile uint32_t t_rx = 0;            // ultima trama de voz recibida
static void despierta_pantalla();             // definida en su seccion
static const int VOLUMEN_JBL = 13;
static volatile uint32_t reabrir_en = 0;      // reabrir el audio si se cierra
static uint32_t heap_arranque = 0, heap_min = UINT32_MAX;

static void apunta_heap()
{
    uint32_t h = esp_get_free_heap_size();
    if (h < heap_min) heap_min = h;
}

static String mac(const uint8_t *b)
{
    char s[18];
    snprintf(s, sizeof s, "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
    return s;
}

static bool lee_mac(const String &t, esp_bd_addr_t out)
{
    int v[6];
    if (sscanf(t.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    return true;
}

// ------------------------------------------------------------------ audio --
/* Dos anillos de bytes: lo que llega del micro y lo que va al altavoz. Los
   rellena y vacía la pila Bluetooth desde SUS tareas, así que aquí no se hace
   nada pesado: Codec2 va en su propia tarea. */
static RingbufHandle_t rb_mic = nullptr, rb_alt = nullptr;
enum : uint8_t { SALIDA_NADA, SALIDA_ECO, SALIDA_TONO };
static volatile uint8_t salida = SALIDA_ECO;
static volatile uint32_t bytes_mic = 0, bytes_alt = 0, huecos_alt = 0;
static volatile uint64_t energia_mic = 0;
static volatile uint32_t muestras_mic = 0;

static volatile uint32_t mic_bloques = 0, mic_vacios = 0;
static void llega_del_micro(const uint8_t *buf, uint32_t len)
{
    bytes_mic += len;
    /* Un bloque TODO a cero no es silencio de verdad (el micro siempre tiene
       algo de ruido): es un paquete SCO perdido que la pila ha rellenado. */
    mic_bloques++;
    bool cero = true;
    for (uint32_t i = 0; i < len && cero; i++) cero = buf[i] == 0;
    if (cero) mic_vacios++;
    if (rb_mic) xRingbufferSend(rb_mic, buf, len, 0);    // si no cabe, se pierde
}

static uint32_t pide_el_altavoz(uint8_t *buf, uint32_t len)
{
    if (!rb_alt) return 0;
    size_t hay = 0;
    vRingbufferGetInfo(rb_alt, nullptr, nullptr, nullptr, nullptr, &hay);
    /* SIEMPRE se entrega lo pedido: si no hay audio listo, silencio. Devolver 0
       o menos de lo pedido era lo que sonaba a chasquidos en reposo. */
    size_t puesto = 0;
    while (puesto < len) {
        size_t n = 0;
        uint8_t *d = (uint8_t *)xRingbufferReceiveUpTo(rb_alt, &n, 0, len - puesto);
        if (!d) break;
        memcpy(buf + puesto, d, n);
        vRingbufferReturnItem(rb_alt, d);
        puesto += n;
    }
    if (puesto < len) { huecos_alt++; memset(buf + puesto, 0, len - puesto); }
    bytes_alt += len;
    return len;
}

/* La pila sólo pide audio de salida cuando se le avisa de que hay. */
static void empuja_salida(void *)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(4));
        if (!audio_abierto || !rb_alt) continue;
        size_t hay = 0;
        vRingbufferGetInfo(rb_alt, nullptr, nullptr, nullptr, nullptr, &hay);
        if (hay >= 120) esp_hf_outgoing_data_ready();
    }
}

static struct Medida { uint32_t n = 0, suma = 0, max = 0;
    void mete(uint32_t u) { n++; suma += u; if (u > max) max = u; }
    uint32_t media() const { return n ? suma / n : 0; } } m_enc, m_dec;

/* Codec2 1200: tramas de 320 muestras (40 ms) y 6 bytes. El ECO guarda 1,5 s
   de tramas codificadas y las devuelve: te oyes a ti mismo con el retraso
   justo para no pisarte, y pasando por el mismo códec que iría por LoRa. */
/* TRAMAS HACIA Y DESDE EL REFLECTOR. Una cola de salida (lo que se habla, ya
   con cabecera) y otra de entrada (tramas de voz de otros), ambas de tramas de
   aire completas. El WiFi sólo lo toca la tarea de red. */
struct Trama { uint8_t len; uint8_t b[8 + 2 + TRAMAS_LOTE * 6 + 16]; };
static QueueHandle_t q_sale = nullptr, q_entra = nullptr;
static volatile uint32_t red_tx = 0, red_rx = 0, red_conexiones = 0;
static volatile bool red_arriba = false;
static volatile bool wifi_quiero = true;
static volatile uint32_t rx_dup = 0;

static void cabecera(Trama &t, uint8_t tipo, uint32_t src, uint8_t stream, uint8_t seq)
{
    t.b[0] = 0xA1; t.b[1] = CANAL; t.b[2] = (tipo << 4) | SALTOS;
    t.b[3] = src >> 16; t.b[4] = src >> 8; t.b[5] = src; t.b[6] = stream; t.b[7] = seq;
    t.len = 8;
}

#ifdef TRANSPORTE_LORA
static volatile int rssi_ultimo = 0;
static void tarea_radio(void *)
{
    SPI.begin(5, 19, 27, 18);
    int st = radio.begin(439.6, 250.0, 7, 5, 0x3B, 17, 8);
    if (st != RADIOLIB_ERR_NONE) { Serial.printf("RADIO: NO arranca (%d)\n", st); vTaskDelete(NULL); }
    radio.setCRC(true);
    radio.setCurrentLimit(140);
    radio.setPacketReceivedAction(al_recibir);
    radio.startReceive();
    red_arriba = true;
    Serial.println("RADIO: LoRa 439,600 MHz SF7 BW250 CR4/5 sync 0x3B, 17 dBm, escuchando");
    for (;;) {
        Trama t;
        if (xQueueReceive(q_sale, &t, 0) == pdTRUE) {
            /* Escuchar antes de hablar, como el nodo: el CAD ve una portadora
               LoRa aunque esté por debajo del ruido. */
            for (int i = 0; i < 3 && radio.scanChannel() != RADIOLIB_CHANNEL_FREE; i++)
                vTaskDelay(pdMS_TO_TICKS(20 + random(40)));
            radio.transmit(t.b, t.len);
            /* DIO0 también sube al acabar de EMITIR: sin esto se leería como
               recibido lo que acabamos de mandar (ver la nota en main.cpp). */
            hay_paquete = false;
            radio.startReceive();
            red_tx++;
            continue;
        }
        if (hay_paquete) {
            hay_paquete = false;
            int len = radio.getPacketLength();
            if (len >= 8 && len <= (int)sizeof t.b && radio.readData(t.b, len) == RADIOLIB_ERR_NONE) {
                t.len = len;
                rssi_ultimo = (int)radio.getRSSI();
                uint8_t tipo = t.b[2] >> 4;
                if (t.b[0] == 0xA1 && (tipo == T_INICIO || tipo == T_VOZ || tipo == T_FIN)) {
                    xQueueSend(q_entra, &t, 0);
                    red_rx++;
                }
            }
            radio.startReceive();
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
#else
static void tarea_red(void *)
{
    WiFi.mode(WIFI_STA);
    /* Una antena para los dos: que el Bluetooth (el audio SCO, que no espera)
       gane los turnos al WiFi, que sí puede esperar unos milisegundos. */
    esp_coex_preference_set(ESP_COEX_PREFER_BT);
    WiFi.begin(WIFI_SSID, WIFI_CLAVE);
    WiFiClient c;
    uint8_t buf[512], kiss[256];
    size_t nk = 0;
    bool dentro = false, esc = false;
    for (;;) {
        static bool wifi_puesto = true;
        if (!wifi_quiero) {
            if (wifi_puesto) { c.stop(); WiFi.disconnect(true); WiFi.mode(WIFI_OFF); wifi_puesto = false; red_arriba = false; Serial.println("WiFi apagado"); }
            vTaskDelay(pdMS_TO_TICKS(200)); continue;
        }
        if (!wifi_puesto) { WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_CLAVE); wifi_puesto = true; Serial.println("WiFi encendido"); }
        if (WiFi.status() != WL_CONNECTED) { red_arriba = false; vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        if (!c.connected()) {
            red_arriba = false;
            if (!c.connect(REFLECTOR, REFLECTOR_PUERTO, 5000)) { vTaskDelay(pdMS_TO_TICKS(3000)); continue; }
            c.setNoDelay(true);
            /* CONVIVENCIA WiFi + audio Bluetooth, medido (15-sep): con el WiFi
               en marcha se pierde el 9,6 % de los bloques del micro (el
               controlador los rellena de ceros), sin WiFi el 0,3 %. Ahorro
               máximo del WiFi (WIFI_PS_MAX_MODEM) lo EMPEORA: 12 %. Se queda el
               ahorro por defecto con preferencia al Bluetooth. El transceptor
               LoRa no tiene este problema: el SX1278 es otra radio. */
            red_conexiones++;
            red_arriba = true;
            Serial.printf("RED: enganchado al reflector %s:%u (IP %s)\n", REFLECTOR, REFLECTOR_PUERTO,
                          WiFi.localIP().toString().c_str());
        }
        Trama t;
        while (xQueueReceive(q_sale, &t, 0) == pdTRUE) {
            uint8_t o[2 * sizeof t.b + 4]; size_t k = 0;
            o[k++] = FEND; o[k++] = CMD_AIRE;
            for (int i = 0; i < t.len; i++) {
                if (t.b[i] == FEND) { o[k++] = FESC; o[k++] = TFEND; }
                else if (t.b[i] == FESC) { o[k++] = FESC; o[k++] = TFESC; }
                else o[k++] = t.b[i];
            }
            o[k++] = FEND;
            c.write(o, k);
            red_tx++;
        }
        int n = c.available() ? c.read(buf, sizeof buf) : 0;
        for (int i = 0; i < n; i++) {
            uint8_t x = buf[i];
            if (x == FEND) {
                if (dentro && nk > 1 && kiss[0] == CMD_AIRE && nk - 1 <= sizeof t.b) {
                    t.len = nk - 1;
                    memcpy(t.b, kiss + 1, t.len);
                    uint8_t tipo = t.b[2] >> 4;
                    if (t.len >= 8 && t.b[0] == 0xA1 && (tipo == T_INICIO || tipo == T_VOZ || tipo == T_FIN)) {
                        xQueueSend(q_entra, &t, 0);
                        red_rx++;
                    }
                }
                dentro = true; nk = 0; esc = false;
                continue;
            }
            if (!dentro) continue;
            if (x == FESC) { esc = true; continue; }
            if (esc) { x = x == TFEND ? FEND : FESC; esc = false; }
            if (nk < sizeof kiss) kiss[nk++] = x;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
#endif

/* NIVELES. El micro del JBL llega unos 30 dB por debajo de lo que Codec2
   necesita (RMS 60-200 hablando, frente a 2.000-5.000 de una voz normal), y
   Codec2 a 1200 exagera tanto lo flojo como lo recortado. Así que:
     - micro: CONTROL AUTOMÁTICO DE GANANCIA hacia RMS 3.000. Baja rápido si se
       pasa (hablar pegado) y sube despacio; con silencio no sube, para no
       convertir el ruido de fondo en un soplido;
     - altavoz: ganancia fija;
     - y los dos con un limitador suave, que recortar a lo bruto suena peor
       que comprimir un poco. */
static float g_mic = 8.0f;
static volatile uint32_t rms_mic_antes = 0, rms_mic_despues = 0;
static const float G_ALTAVOZ = 1.8f;
static volatile uint32_t pico_mic = 0;

static inline short limita(float x)
{
    const float U = 24000.0f;                 // a partir de aquí, comprimir
    float a = x < 0 ? -x : x;
    if (a > U) a = U + (32767.0f - U) * (1.0f - exp(-(a - U) / (32767.0f - U)));
    return (short)(x < 0 ? -a : a);
}

static void agc_micro(short *pcm, int n)
{
    double e = 0;
    for (int i = 0; i < n; i++) e += (double)pcm[i] * pcm[i];
    float rms = sqrt(e / n);
    rms_mic_antes = rms;
    for (int i = 0; i < n; i++) { uint32_t a = abs(pcm[i]); if (a > pico_mic) pico_mic = a; }
    /* PUERTA DE RUIDO: por debajo de esto no es voz, y subirlo sólo fabrica
       soplido. Se atenúa en vez de amplificar. */
    if (rms < 60) {
        for (int i = 0; i < n; i++) pcm[i] = pcm[i] / 4;
        rms_mic_despues = rms / 4;
        return;
    }
    {
        float objetivo = 3000.0f / rms;
        if (objetivo > 12.0f) objetivo = 12.0f;
        if (objetivo < 0.5f) objetivo = 0.5f;
        if (objetivo < g_mic) g_mic = objetivo;                    // bajar ya
        else g_mic += (objetivo - g_mic) * 0.05f;                  // subir poco a poco
    }
    double e2 = 0;
    for (int i = 0; i < n; i++) { pcm[i] = limita(pcm[i] * g_mic); e2 += (double)pcm[i] * pcm[i]; }
    rms_mic_despues = sqrt(e2 / n);
}

/* TRANSCEPTOR SIMPLEX. PTT abajo: lo del micro se codifica y sale al reflector
   en lotes de 480 ms, como la app. PTT arriba: lo que llega de la red se
   descodifica al altavoz a ritmo real, con medio lote de colchón. */
static void tarea_codec(void *)
{
    /* SIMPLEX TAMBIÉN EN MEMORIA: sólo vive el códec de la dirección que toca.
       Con WiFi + Bluetooth Classic, los ~30 KB del otro son los que faltan. */
    struct CODEC2 *enc = nullptr;
    struct CODEC2 *dec = codec2_create(CODEC2_MODE_1200);
    codec2_set_natural_or_gray(dec, 1);
    apunta_heap();
    const int N = 320, B = 6;
    const uint32_t mi_src = hash_indicativo(INDICATIVO);
    static short pcm[320], sal[320];
    static uint8_t acum[640];
    static uint8_t cola_rx[3000];              // tramas de códec recibidas por sonar
    size_t en_acum = 0, rx_hay = 0, rx_pos = 0;
    bool ptt_antes = false, sonando = false;
    /* LA MISMA VOZ LLEGA VARIAS VECES: la app la manda por el nodo y por el
       camino de datos, y el reflector reparte las dos copias. Se descarta por
       (src, stream, seq), igual que hace el nodo. */
    uint32_t rx_src = 0; uint8_t rx_stream = 0; int rx_ultimo_seq = -1;
    uint8_t stream = 0, seq = 0, en_lote = 0;
    Trama lote;
    TickType_t marca = xTaskGetTickCount();

    for (;;) {
        size_t n = 0;
        uint8_t *d = (uint8_t *)xRingbufferReceiveUpTo(rb_mic, &n, pdMS_TO_TICKS(5), sizeof acum - en_acum);
        if (d) { memcpy(acum + en_acum, d, n); vRingbufferReturnItem(rb_mic, d); en_acum += n; }

        if (ptt && !ptt_antes) {
            if (dec) { codec2_destroy(dec); dec = nullptr; }
            enc = codec2_create(CODEC2_MODE_1200);
            if (enc) codec2_set_natural_or_gray(enc, 1);
            apunta_heap();
            sonando = false; rx_hay = rx_pos = 0;
            stream = (uint8_t)random(1, 256); seq = 0; en_lote = 0;
            Trama t; cabecera(t, T_INICIO, mi_src, stream, 0);
            t.b[t.len++] = C2_1200;
            size_t li = strlen(INDICATIVO); memcpy(t.b + t.len, INDICATIVO, li); t.len += li;
            t.b[t.len++] = 0;
            xQueueSend(q_sale, &t, 0);
            ptt_desde = millis(); tot_cortado = false;
            despierta_pantalla();
            Serial.println("PTT abajo: al aire por el reflector");
        }
        if (!ptt && ptt_antes) {
            if (en_lote) { lote.b[9] = en_lote; lote.len = 10 + en_lote * B; xQueueSend(q_sale, &lote, 0); }
            Trama t; cabecera(t, T_FIN, mi_src, stream, ++seq);
            xQueueSend(q_sale, &t, 0);
            if (enc) { codec2_destroy(enc); enc = nullptr; }
            dec = codec2_create(CODEC2_MODE_1200);
            if (dec) codec2_set_natural_or_gray(dec, 1);
            apunta_heap();
            ptt_desde = 0;
            despierta_pantalla();
            Serial.printf("PTT arriba (micro: pico %u, ganancia final x%.1f, vacios %u/%u)\n",
                          pico_mic, g_mic, mic_vacios, mic_bloques);
            pico_mic = 0;
        }
        ptt_antes = ptt;

        if (en_acum >= (size_t)N * 2) {
            memcpy(pcm, acum, N * 2);
            memmove(acum, acum + N * 2, en_acum - N * 2);
            en_acum -= N * 2;
            for (int i = 0; i < N; i++) energia_mic += (int32_t)pcm[i] * pcm[i];
            muestras_mic += N;
            if (ptt && enc) {
                agc_micro(pcm, N);
                if (en_lote == 0) { cabecera(lote, T_VOZ, mi_src, stream, ++seq); lote.b[8] = C2_1200; }
                uint32_t t0 = micros();
                codec2_encode(enc, lote.b + 10 + en_lote * B, pcm);
                m_enc.mete(micros() - t0);
                if (++en_lote == TRAMAS_LOTE) {
                    lote.b[9] = en_lote; lote.len = 10 + en_lote * B;
                    xQueueSend(q_sale, &lote, 0);
                    en_lote = 0;
                }
            }
        }

        /* Lo que llega de la red. Con el PTT abajo se tira: simplex. */
        Trama t;
        while (xQueueReceive(q_entra, &t, 0) == pdTRUE) {
            uint8_t tipo = t.b[2] >> 4;
            if (ptt) continue;
            uint32_t src = ((uint32_t)t.b[3] << 16) | ((uint32_t)t.b[4] << 8) | t.b[5];
            uint8_t stm = t.b[6], sq = t.b[7];
            if (src == mi_src) continue;               // lo nuestro, repetido por la celda
            bool mismo = src == rx_src && stm == rx_stream;
            if (tipo == T_INICIO) {
                if (mismo) continue;                          // copia del INICIO
                char ind[16] = {0};
                memcpy(ind, t.b + 9, t.len > 9 ? min((int)t.len - 9, 15) : 0);
#ifdef TRANSPORTE_LORA
                Serial.printf("RX: habla %s (%d dBm)\n", ind, rssi_ultimo);
#else
                Serial.printf("RX: habla %s\n", ind);
#endif
                /* "%.14s" y no "%s": el ultimo byte del buffer no se escribe
                   NUNCA, asi que la pantalla (que lee desde otra tarea) siempre
                   encuentra el final de la cadena aunque pille el relevo. */
                snprintf(hablante, sizeof hablante, "%.14s", ind[0] ? ind : "?");
#ifdef TRANSPORTE_LORA
                hablante_rssi = rssi_ultimo;
#endif
                t_rx = millis();
                despierta_pantalla();
                rx_src = src; rx_stream = stm; rx_ultimo_seq = 0;
                rx_hay = rx_pos = 0; sonando = false;
                continue;
            }
            if (!mismo || (int)sq <= rx_ultimo_seq) { rx_dup++; continue; }   // copia o de otro
            rx_ultimo_seq = sq;
            if (tipo == T_VOZ && t.len >= 10 && t.b[8] == C2_1200) {
                uint8_t nt = t.b[9];
                if (rx_pos && rx_pos == rx_hay) rx_hay = rx_pos = 0;
                for (int i = 0; i < nt && 10 + (i + 1) * B <= t.len && rx_hay + B <= sizeof cola_rx; i++, rx_hay += B)
                    memcpy(cola_rx + rx_hay, t.b + 10 + i * B, B);
                t_rx = millis();
#ifdef TRANSPORTE_LORA
                hablante_rssi = rssi_ultimo;
#endif
                if (!sonando && rx_hay - rx_pos >= (size_t)6 * B) { sonando = true; marca = xTaskGetTickCount(); }
            } else if (tipo == T_FIN) {
                if (!sonando && rx_hay > rx_pos) sonando = true;
            }
        }

        if (xTaskGetTickCount() - marca >= pdMS_TO_TICKS(40)) {
            marca += pdMS_TO_TICKS(40);
            if (sonando && dec && rx_pos + B <= rx_hay) {
                uint32_t t0 = micros();
                codec2_decode(dec, sal, cola_rx + rx_pos);
                m_dec.mete(micros() - t0);
                for (int i = 0; i < N; i++) sal[i] = limita(sal[i] * G_ALTAVOZ);
                rx_pos += B;
                if (rx_pos >= rx_hay) { sonando = false; }
            } else {
                memset(sal, 0, sizeof sal);
            }
            xRingbufferSend(rb_alt, sal, N * 2, 0);
            apunta_heap();
        }
        vTaskDelay(1);
    }
}

// --------------------------------------------------------------- pantalla --
//
// Se dibuja siempre, haya micro enganchado o no: la pantalla es lo primero que
// se mira cuando algo no suena, y tiene que poder decir "no hay micro".

enum : uint8_t { PANTALLA_AUTO, PANTALLA_FIJA, PANTALLA_OFF };
static uint8_t pantalla_modo = PANTALLA_AUTO;
/* El apagado cuenta desde que se ACABA lo que había que mirar, no desde que
   empieza: mientras se transmite o se recibe la pantalla no se apaga nunca, y
   el medio minuto corre cuando el otro suelta el PTT. */
static const uint32_t PANTALLA_MS = 30000;
static bool hay_oled = false, pantalla_on = true, redibujar = true;
static uint32_t t_actividad = 0, t_pintada = 0;
static Adafruit_SSD1306 oled(128, 64, &Wire, -1);

/* Cualquier cosa que merezca mirarse enciende la pantalla: el PTT, alguien que
   empieza a hablar, el micro que aparece o se va, el boton. */
static void despierta_pantalla()
{
    t_actividad = millis();
    if (!pantalla_on) { pantalla_on = true; redibujar = true; }
}

static void gestiona_pantalla()
{
    if (!hay_oled) return;
    /* TX y RX mandan sobre el reloj: una transmisión de dos minutos o una
       ráfaga larga del otro tienen que verse enteras. */
    if (ptt || millis() - t_rx < 1500) despierta_pantalla();
    bool debe = pantalla_modo == PANTALLA_FIJA ||
                (pantalla_modo == PANTALLA_AUTO && millis() - t_actividad < PANTALLA_MS);
    if (pantalla_modo == PANTALLA_OFF) debe = false;
    if (debe == pantalla_on) return;
    pantalla_on = debe;
    // Apagar el panel sin reinicializarlo: consume casi cero y vuelve al instante.
    oled.ssd1306_command(pantalla_on ? SSD1306_DISPLAYON : SSD1306_DISPLAYOFF);
    if (pantalla_on) redibujar = true;
}

/* LA BATERIA, y con la misma cautela que el nodo: GPIO35 FLOTA si no hay pila
   puesta y entonces se inventa un porcentaje distinto cada rato. Lo que delata
   al pin al aire no es el ruido rapido —eso lo promedia el ADC— sino que el
   valor DERIVA en el curso de MINUTOS; una bateria de verdad es una fuente de
   tension y no hace eso. Asi que se guarda una lectura por minuto y, si el
   recorrido pasa de 100 mV, se devuelve 0 = "no lo se" y la pantalla no enseña
   nada. Y hasta tener cuatro lecturas tampoco se enseña: mejor un hueco los
   primeros minutos que un 79% inventado (que es lo que salio al probarlo con la
   placa alimentada por USB, el 20-sep). */
static uint8_t bateria_pct()
{
    static uint32_t t_valor = 0, t_hist = 0, mv = 0;
    static uint32_t hist[16] = {0};
    static uint8_t nh = 0, llenas = 0;
    if (!mv || millis() - t_valor > 15000) {
        t_valor = millis();
        uint32_t suma = 0;
        for (int i = 0; i < 8; i++) { suma += analogRead(P_BAT); delayMicroseconds(200); }
        mv = (uint32_t)((suma / 8) * 2 * 3300.0 / 4095.0);
        if (!t_hist || millis() - t_hist > 60000) {
            t_hist = millis();
            hist[nh] = mv; nh = (nh + 1) % 16; if (llenas < 16) llenas++;
        }
    }
    if (llenas < 4) return 0;                 // aun no hay con que juzgarlo
    uint32_t mn = 0xFFFFFFFF, mx = 0;
    for (uint8_t i = 0; i < llenas; i++) { if (hist[i] < mn) mn = hist[i]; if (hist[i] > mx) mx = hist[i]; }
    if (mx - mn > 100) return 0;              // deriva: el pin esta al aire
    if (mv <= 3300) return 0;
    if (mv >= 4200) return 100;
    return (mv - 3300) * 100 / 900;
}

/* EL S-METRO. Un numero en dBm solo lo lee quien ya sabe leerlo; la barra dice
   de un vistazo si queda margen. Ocho segmentos entre -110 (por debajo no se
   descodifica) y -55 dBm (por encima da igual cuanto sobre). */
#ifdef TRANSPORTE_LORA
static void smetro(int x, int y, int dbm)
{
    int n = (dbm + 110) / 7;
    if (n < 0) n = 0;
    if (n > 8) n = 8;
    for (int i = 0; i < 8; i++) {
        if (i < n) oled.fillRect(x + i * 6, y, 5, 7, SSD1306_WHITE);
        else       oled.drawRect(x + i * 6, y, 5, 7, SSD1306_WHITE);
    }
}
#endif

/* LA PANTALLA, POR LA CONSOLA. El transceptor se prueba muchas veces desde
   otro sitio (la placa en la mesa, uno por SSH), y entonces "¿que pone?" no se
   puede contestar. Esto vuelca el framebuffer en texto, un caracter por punto:
   128 columnas por 64 lineas. Sale ancho a proposito — juntando puntos de dos
   en dos las letras se empastan y no se lee nada, probado el 20-sep. */
static void captura()
{
    if (!hay_oled) { Serial.println("no hay pantalla que mirar"); return; }
    uint8_t *b = oled.getBuffer();
    char raya[131];
    memset(raya, '-', 130); raya[0] = raya[129] = '+'; raya[130] = 0;
    Serial.println(raya);
    for (int y = 0; y < 64; y++) {
        char l[131];
        int k = 0;
        l[k++] = '|';
        for (int x = 0; x < 128; x++)
            l[k++] = (b[(y / 8) * 128 + x] & (1 << (y % 8))) ? '#' : ' ';
        l[k++] = '|';
        l[k] = 0;
        Serial.println(l);
    }
    Serial.println(raya);
}

static void pinta()
{
    if (!hay_oled || !pantalla_on) return;
    uint32_t ahora = millis();
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);

    /* Arriba, lo que no cambia: quien eres y cuanta pila queda. */
    oled.setCursor(0, 0);
    oled.print(INDICATIVO);
    uint8_t bat = bateria_pct();
    if (bat) {
        char b[8];
        snprintf(b, sizeof b, "%u%%", bat);
        oled.setCursor(128 - 6 * (int)strlen(b), 0);
        oled.print(b);
    }
    oled.drawFastHLine(0, 10, 128, SSD1306_WHITE);

    /* EL ESTADO, en grande: es lo unico que se lee con la radio en el cinturon
       y de reojo. TX / RX / -- , y al lado el detalle. */
    bool rx = !ptt && (ahora - t_rx < 1500);
    oled.setTextSize(2);
    oled.setCursor(0, 14);
    oled.print(ptt ? "TX" : (rx ? "RX" : "--"));
    oled.setTextSize(1);
    oled.setCursor(34, 14);
    if (ptt) {
        uint32_t s = (ahora - ptt_desde) / 1000;
        uint32_t queda = TOT_MS / 1000 > s ? TOT_MS / 1000 - s : 0;
        oled.printf("al aire %lus", (unsigned long)s);
        oled.setCursor(34, 24);
        /* El aviso del TOT parpadea el ultimo cuarto de minuto: si te callan a
           mitad de frase tienes que haberlo visto venir. */
        if (queda <= 15) { if ((ahora / 300) & 1) oled.printf("TOT EN %lus", (unsigned long)queda); }
        else oled.printf("queda %lu:%02lu", (unsigned long)(queda / 60), (unsigned long)(queda % 60));
    } else if (rx) {
        oled.print(hablante[0] ? hablante : "alguien");
        oled.setCursor(34, 24);
#ifdef TRANSPORTE_LORA
        oled.printf("%d", hablante_rssi);
        smetro(80, 24, hablante_rssi);
#else
        oled.print("por el reflector");
#endif
    } else {
        oled.print(tot_cortado ? "cortado por TOT" : "en escucha");
        oled.setCursor(34, 24);
        if (hablante[0]) {
            oled.print("ult ");
            oled.print(hablante);
#ifdef TRANSPORTE_LORA
            oled.printf(" %d", hablante_rssi);
#endif
        } else oled.print("sin trafico aun");
    }

    /* EL MICRO BLUETOOTH. Sin el no hay transceptor, y cuando falla lo que hay
       que saber es EN QUE PASO se quedo: si no aparece, si aparecio pero no hay
       canal de audio, o si esta entero. */
    oled.setCursor(0, 36);
    if (!slc)               oled.print(hay_par ? "micro: buscando..." : "micro: sin vincular");
    else if (!audio_abierto) oled.print("micro: sin audio");
    else {
        oled.print("micro: ");
        char n[16];
        snprintf(n, sizeof n, "%.14s", nombre_par[0] ? nombre_par : "listo");
        oled.print(n);
    }

    /* El canal, que es lo unico que dos equipos tienen que compartir. */
    oled.setCursor(0, 46);
#ifdef TRANSPORTE_LORA
    oled.printf("439.600 sf7 17dBm");
#else
    oled.print(red_arriba ? "reflector: enganchado" : "reflector: sin red");
#endif

    oled.setCursor(0, 56);
    oled.printf("tx%lu rx%lu ch%u %uk", (unsigned long)red_tx, (unsigned long)red_rx,
                (unsigned)CANAL, (unsigned)(esp_get_free_heap_size() / 1024));

    oled.display();
    redibujar = false;
    t_pintada = ahora;
}

// --------------------------------------------------------------- manos libres --
static const char *EST_CON[] = { "desconectado", "conectando", "conectado", "SLC listo", "desconectando" };
static const char *EST_AUD[] = { "cerrado", "abriendo", "abierto (CVSD 8 kHz)", "abierto (mSBC 16 kHz)" };

static void manos_libres(esp_hf_cb_event_t ev, esp_hf_cb_param_t *p)
{
    switch (ev) {
    case ESP_HF_CONNECTION_STATE_EVT:
        Serial.printf("HFP conexion: %s (%s) rasgos 0x%x\n",
                      p->conn_stat.state <= 4 ? EST_CON[p->conn_stat.state] : "?",
                      mac(p->conn_stat.remote_bda).c_str(), p->conn_stat.peer_feat);
        slc = p->conn_stat.state == ESP_HF_CONNECTION_STATE_SLC_CONNECTED;
        despierta_pantalla();
        if (slc) {
            memcpy(par, p->conn_stat.remote_bda, 6);
            hay_par = true;
            prefs.putBytes("par", par, 6);
            esp_bt_hf_volume_control(par, ESP_HF_VOLUME_CONTROL_TARGET_SPK, VOLUMEN_JBL);
            esp_bt_hf_volume_control(par, ESP_HF_VOLUME_CONTROL_TARGET_MIC, 15);
            /* Se abre el audio en cuanto hay conexión: un transceptor tiene
               que estar escuchando siempre, no sólo durante una "llamada". */
            esp_bt_hf_connect_audio(par);
        }
        break;
    case ESP_HF_AUDIO_STATE_EVT:
        Serial.printf("HFP audio: %s\n", p->audio_stat.state <= 3 ? EST_AUD[p->audio_stat.state] : "?");
        audio_abierto = p->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED ||
                        p->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED_MSBC;
        despierta_pantalla();
        if (audio_abierto)
            esp_bt_hf_register_data_callback(llega_del_micro, pide_el_altavoz);
        /* Un transceptor escucha SIEMPRE: si el auricular cierra el audio (el
           JBL lo hace al pulsar "tel"), se vuelve a abrir. */
        else if (p->audio_stat.state == ESP_HF_AUDIO_STATE_DISCONNECTED && slc)
            reabrir_en = millis() + 700;
        break;
    /* LOS BOTONES. Cada uno de estos es un candidato a PTT. */
    case ESP_HF_BVRA_RESPONSE_EVT:
        Serial.printf("BOTON: asistente de voz -> %s\n", p->vra_rep.value ? "ON" : "OFF");
        ptt = p->vra_rep.value;
        /* Se le confirma, para que el auricular crea que el "asistente" está
           activo y la siguiente pulsación mande el OFF. */
        esp_bt_hf_vra(par, p->vra_rep.value ? ESP_HF_VR_STATE_ENABLED : ESP_HF_VR_STATE_DISABLED);
        if (!audio_abierto) reabrir_en = millis() + 300;
        break;
    case ESP_HF_ATA_RESPONSE_EVT:
        Serial.println("BOTON: contestar (ATA)");
        esp_bt_hf_cmee_response(p->ata_rep.remote_addr, ESP_HF_AT_RESPONSE_CODE_OK, ESP_HF_CME_AG_FAILURE);
        break;
    case ESP_HF_CHUP_RESPONSE_EVT:
        Serial.println("BOTON: colgar (AT+CHUP)");
        esp_bt_hf_cmee_response(p->chup_rep.remote_addr, ESP_HF_AT_RESPONSE_CODE_OK, ESP_HF_CME_AG_FAILURE);
        break;
    case ESP_HF_DIAL_EVT:
        Serial.printf("BOTON: marcar (%s)\n", p->out_call.num_or_loc ? p->out_call.num_or_loc : "rellamada");
        esp_bt_hf_cmee_response(p->out_call.remote_addr, ESP_HF_AT_RESPONSE_CODE_OK, ESP_HF_CME_AG_FAILURE);
        break;
    case ESP_HF_VOLUME_CONTROL_EVT: {
        Serial.printf("BOTON: volumen %s = %d\n", p->volume_control.type ? "micro" : "altavoz",
                      p->volume_control.volume);
        /* PTT PROVISIONAL con los botones de volumen, que son los únicos que el
           JBL manda siempre (el "tel" sin llamada casi nunca manda nada):
           bajar = PTT abajo, subir = PTT arriba. El Abbree traerá el suyo. */
        static int vol_antes = -1;
        int v = p->volume_control.volume;
        if (p->volume_control.type == ESP_HF_VOLUME_CONTROL_TARGET_SPK) {
            if (vol_antes >= 0 && v < vol_antes && !ptt) ptt = true;
            else if (vol_antes >= 0 && v > vol_antes && ptt) ptt = false;
            /* Se devuelve el volumen a 13 tras cada pulsación: alto para oír
               bien, y con margen para que el botón siga mandando eventos (en el
               tope de 15 el "+" ya no manda nada y el PTT se quedaría pulsado). */
            if (v != VOLUMEN_JBL) esp_bt_hf_volume_control(par, ESP_HF_VOLUME_CONTROL_TARGET_SPK, VOLUMEN_JBL);
            vol_antes = VOLUMEN_JBL;
        }
        break;
    }
    case ESP_HF_UNAT_RESPONSE_EVT:
        /* Órdenes que no son del estándar: aquí suelen ir los PTT de los micros
           para POC (tipo AT+PTT=P / AT+PTT=R). Se apuntan tal cual. */
        Serial.printf("BOTON: AT desconocida \"%s\"\n", p->unat_rep.unat ? p->unat_rep.unat : "");
        esp_hf_unat_response(p->unat_rep.remote_addr, NULL);
        break;
    case ESP_HF_CIND_RESPONSE_EVT:
        esp_bt_hf_cind_response(p->cind_rep.remote_addr, ESP_HF_CALL_STATUS_NO_CALLS,
                                ESP_HF_CALL_SETUP_STATUS_IDLE, ESP_HF_NETWORK_STATE_AVAILABLE,
                                5, ESP_HF_ROAMING_STATUS_INACTIVE, 5, ESP_HF_CALL_HELD_STATUS_NONE);
        break;
    case ESP_HF_COPS_RESPONSE_EVT:
        esp_bt_hf_cops_response(p->cops_rep.remote_addr, (char *)"PTT LoRa");
        break;
    case ESP_HF_CLCC_RESPONSE_EVT:
        esp_bt_hf_cmee_response(p->clcc_rep.remote_addr, ESP_HF_AT_RESPONSE_CODE_OK, ESP_HF_CME_AG_FAILURE);
        break;
    case ESP_HF_CNUM_RESPONSE_EVT:
        esp_bt_hf_cmee_response(par, ESP_HF_AT_RESPONSE_CODE_OK, ESP_HF_CME_AG_FAILURE);
        break;
    case ESP_HF_VTS_RESPONSE_EVT:
        Serial.printf("BOTON: DTMF %s\n", p->vts_rep.code);
        break;
    case ESP_HF_NREC_RESPONSE_EVT:
        Serial.printf("HFP: reduccion de eco del auricular %s\n", p->nrec.state ? "ON" : "OFF");
        break;
    case ESP_HF_BCS_RESPONSE_EVT:
        Serial.printf("HFP: codec negociado %d\n", p->bcs_rep.mode);
        break;
    default:
        Serial.printf("HFP evento %d\n", ev);
        break;
    }
}

// -------------------------------------------------------------------- GAP --
static void gap(esp_bt_gap_cb_event_t ev, esp_bt_gap_cb_param_t *p)
{
    switch (ev) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        char nombre[64] = "";
        uint32_t cod = 0;
        int rssi = 0;
        for (int i = 0; i < p->disc_res.num_prop; i++) {
            esp_bt_gap_dev_prop_t &pr = p->disc_res.prop[i];
            if (pr.type == ESP_BT_GAP_DEV_PROP_COD) cod = *(uint32_t *)pr.val;
            else if (pr.type == ESP_BT_GAP_DEV_PROP_RSSI) rssi = *(int8_t *)pr.val;
            else if (pr.type == ESP_BT_GAP_DEV_PROP_BDNAME)
                snprintf(nombre, sizeof nombre, "%.*s", pr.len, (char *)pr.val);
            else if (pr.type == ESP_BT_GAP_DEV_PROP_EIR && !nombre[0]) {
                uint8_t l = 0;
                uint8_t *n = esp_bt_gap_resolve_eir_data((uint8_t *)pr.val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &l);
                if (!n) n = esp_bt_gap_resolve_eir_data((uint8_t *)pr.val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &l);
                if (n) snprintf(nombre, sizeof nombre, "%.*s", l, (char *)n);
            }
        }
        /* Clase "audio/vídeo" (0x04) = auriculares, altavoces, manos libres. */
        bool audio = ((cod >> 8) & 0x1F) == 0x04;
        Serial.printf("  %s  %4d dBm  %-28s %s\n", mac(p->disc_res.bda).c_str(), rssi,
                      nombre[0] ? nombre : "(sin nombre)", audio ? "<- audio" : "");
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        Serial.println(p->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED ? "buscando..." : "fin de la busqueda");
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        Serial.printf("emparejado con %s: %s\n", p->auth_cmpl.device_name,
                      p->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS ? "OK" : "FALLO");
        if (p->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            /* El nombre es para la PANTALLA: con dos micros en la mesa, una MAC
               no dice cual esta enganchado. Se recuerda como la MAC. */
            snprintf(nombre_par, sizeof nombre_par, "%.23s", (const char *)p->auth_cmpl.device_name);
            prefs.putString("nom", nombre_par);
            despierta_pantalla();
        }
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
        esp_bt_gap_pin_reply(p->pin_req.bda, true, 4, pin);
        Serial.println("PIN pedido: 0000");
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
        esp_bt_gap_ssp_confirm_reply(p->cfm_req.bda, true);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- consola --
static void estado()
{
    uint32_t rms = muestras_mic ? (uint32_t)sqrt((double)energia_mic / muestras_mic) : 0;
    Serial.printf("heap libre %u (al arrancar %u, minimo %u) · par %s · SLC %s · audio %s · "
                  "micro %u B (rms %u) · altavoz %u B, huecos %u · codificar %u us (max %u) · "
                  "descodificar %u us (max %u) · red %s tx %u rx %u copias %u (conexiones %u) · micro vacios %u/%u\n",
                  esp_get_free_heap_size(), heap_arranque, heap_min,
                  hay_par ? mac(par).c_str() : "ninguno", slc ? "si" : "no",
                  audio_abierto ? "abierto" : "cerrado", bytes_mic, rms, bytes_alt, huecos_alt,
                  m_enc.media(), m_enc.max, m_dec.media(), m_dec.max,
                  red_arriba ? "arriba" : "abajo", red_tx, red_rx, rx_dup, red_conexiones, mic_vacios, mic_bloques);
}

static void orden(String l)
{
    l.trim();
    if (l == "buscar") {
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 8, 0);
    } else if (l.startsWith("conecta")) {
        esp_bd_addr_t b;
        if (lee_mac(l.substring(8), b)) {
            memcpy(par, b, 6);
            hay_par = true;
            esp_bt_gap_cancel_discovery();
            Serial.printf("conectando con %s\n", mac(b).c_str());
            esp_bt_hf_connect(b);
        } else Serial.println("uso: conecta AA:BB:CC:DD:EE:FF");
    } else if (l == "audio" && hay_par) {
        esp_bt_hf_connect_audio(par);
    } else if (l == "corta" && hay_par) {
        esp_bt_hf_disconnect_audio(par);
    } else if (l == "ptt") {
        ptt = !ptt; Serial.printf("PTT por consola: %s\n", ptt ? "abajo" : "arriba");
    } else if (l == "tono") {
        salida = SALIDA_TONO; Serial.println("salida: tono de 800 Hz");
    } else if (l == "nada") {
        salida = SALIDA_NADA; Serial.println("salida: silencio");
#ifndef TRANSPORTE_LORA
    } else if (l == "wifi off") {
        wifi_quiero = false;
    } else if (l == "wifi on") {
        wifi_quiero = true;
#endif
    } else if (l == "captura") {
        Serial.printf("pantalla: %s, modo %s\n", pantalla_on ? "encendida" : "APAGADA",
                      pantalla_modo == PANTALLA_FIJA ? "fija" :
                      (pantalla_modo == PANTALLA_OFF ? "off" : "auto"));
        pinta();                       // si está apagada, no repinta: se ve lo último
        captura();
    } else if (l.startsWith("pantalla")) {
        String m = l.substring(8); m.trim();
        if (m == "fija") pantalla_modo = PANTALLA_FIJA;
        else if (m == "off") pantalla_modo = PANTALLA_OFF;
        else pantalla_modo = PANTALLA_AUTO;
        despierta_pantalla();
        Serial.printf("pantalla: %s\n", pantalla_modo == PANTALLA_FIJA ? "siempre encendida" :
                      (pantalla_modo == PANTALLA_OFF ? "apagada" : "auto (30 s tras el ultimo TX o RX)"));
    } else if (l == "cuenta") {
        mic_vacios = 0; mic_bloques = 0; Serial.println("contadores del micro a cero");
    } else if (l == "estado") {
        estado();
    } else if (l.length()) {
        Serial.println("ordenes: buscar · conecta MAC · audio · corta · eco · tono · nada · "
                       "pantalla [fija|off|auto] · captura · estado");
    }
}

void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== PTT LoRa · banco manos libres (HFP-AG) ===");
    /* Cada analogRead() del medidor de bateria imprime una linea del driver de
       GPIO; ocho por medida y una medida cada 10 s tapan lo que importa. */
    esp_log_level_set("gpio", ESP_LOG_WARN);
    prefs.begin("hfpag", false);
    hay_par = prefs.getBytes("par", par, 6) == 6;
    prefs.getString("nom", nombre_par, sizeof nombre_par);

    /* La pantalla, lo primero: si algo de abajo se atasca (el controlador
       Bluetooth no arranca, la radio no responde), al menos se ve que la placa
       ha encendido y por donde iba. */
    Wire.begin(P_SDA, P_SCL, 400000);
    hay_oled = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    Serial.printf("pantalla OLED: %s\n", hay_oled ? "si" : "NO la veo");
    if (hay_oled) {
        oled.clearDisplay();
        oled.setTextColor(SSD1306_WHITE);
        oled.setTextSize(2);
        oled.setCursor(0, 8);
        oled.print("PTT LoRa");
        oled.setTextSize(1);
        oled.setCursor(0, 32);
        oled.print("transceptor");
        oled.setCursor(0, 44);
        oled.print(INDICATIVO);
        oled.setCursor(0, 56);
        oled.print("arrancando...");
        oled.display();
    }
    pinMode(P_BOTON, INPUT_PULLUP);
    despierta_pantalla();

    esp_err_t e = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    Serial.printf("liberar BLE: %s\n", esp_err_to_name(e));
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((e = esp_bt_controller_init(&cfg)) != ESP_OK) { Serial.printf("NO arranca: controlador init %s\n", esp_err_to_name(e)); return; }
    if ((e = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) { Serial.printf("NO arranca: controlador enable %s\n", esp_err_to_name(e)); return; }
    if ((e = esp_bluedroid_init()) != ESP_OK) { Serial.printf("NO arranca: bluedroid init %s\n", esp_err_to_name(e)); return; }
    if ((e = esp_bluedroid_enable()) != ESP_OK) { Serial.printf("NO arranca: bluedroid enable %s\n", esp_err_to_name(e)); return; }
    esp_bt_dev_set_device_name("PTT LoRa");
    esp_bt_gap_register_callback(gap);
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
    esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin);
    esp_bt_hf_register_callback(manos_libres);
    esp_bt_hf_init(par);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    rb_mic = xRingbufferCreate(3072, RINGBUF_TYPE_BYTEBUF);
    rb_alt = xRingbufferCreate(3072, RINGBUF_TYPE_BYTEBUF);
    q_sale = xQueueCreate(8, sizeof(Trama));
    q_entra = xQueueCreate(8, sizeof(Trama));
    xTaskCreatePinnedToCore(tarea_codec, "codec", 22 * 1024, nullptr, 5, nullptr, 1);
#ifdef TRANSPORTE_LORA
    xTaskCreatePinnedToCore(tarea_radio, "radio", 6 * 1024, nullptr, 4, nullptr, 1);
#else
    xTaskCreatePinnedToCore(tarea_red, "red", 6 * 1024, nullptr, 4, nullptr, 1);
#endif
    xTaskCreatePinnedToCore(empuja_salida, "salida", 3 * 1024, nullptr, 6, nullptr, 1);

    heap_arranque = esp_get_free_heap_size();
    apunta_heap();
    Serial.printf("Bluetooth Classic listo como \"PTT LoRa\", heap libre %u\n", heap_arranque);
    if (hay_par) {
        Serial.printf("reconectando con %s (el ultimo)\n", mac(par).c_str());
        esp_bt_hf_connect(par);
    } else {
        Serial.println("pon el altavoz en modo emparejar y escribe: buscar");
    }
}

void loop()
{
    static String l;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') { orden(l); l = ""; }
        else l += c;
    }
    /* Si el auricular no está (apagado, fuera de alcance), se insiste cada 15 s:
       encenderlo tiene que bastar para que el transceptor lo coja. */
    static uint32_t t_reintento = 0;
    if (!slc && hay_par && millis() - t_reintento > 15000) {
        t_reintento = millis();
        Serial.printf("buscando al auricular %s...\n", mac(par).c_str());
        esp_bt_hf_connect(par);
    }
    if (reabrir_en && millis() >= reabrir_en && slc && !audio_abierto && hay_par) {
        reabrir_en = 0;
        Serial.println("reabriendo el audio");
        esp_bt_hf_connect_audio(par);
    }
    /* EL TOT, aqui y no en la tarea del codec: el PTT de un micro Bluetooth es
       un interruptor, no un pulsador — si se queda puesto (el boton mal dado, el
       micro en el bolsillo) nadie lo suelta. Al cortar queda constancia en la
       pantalla, que si no parece que se ha estropeado. */
    if (ptt && ptt_desde && millis() - ptt_desde > TOT_MS) {
        ptt = false;
        tot_cortado = true;
        despierta_pantalla();
        Serial.println("TOT: se acabo el tiempo de transmision, microfono cerrado");
    }

    /* El boton de fabrica despierta la pantalla. No hace de PTT a proposito:
       para eso esta el del micro, y un PTT en la placa obligaria a tenerla en
       la mano, que es justo lo que este cacharro evita. */
    static uint32_t t_boton = 0;
    if (digitalRead(P_BOTON) == LOW && millis() - t_boton > 300) {
        t_boton = millis();
        despierta_pantalla();
    }

    gestiona_pantalla();
    /* Se repinta cuando hay algo que contar (redibujar), cada 250 ms mientras
       se habla o se escucha —el contador de segundos y el S-metro se mueven— y
       cada segundo en reposo. Volcar el framebuffer por I2C a 400 kHz son unos
       23 ms de una tarea de prioridad minima: el audio ni se entera. */
    {
        bool moviendose = ptt || (millis() - t_rx < 1500);
        uint32_t cada = moviendose ? 250 : 1000;
        if (redibujar || millis() - t_pintada > cada) pinta();
    }

    static uint32_t t = 0;
    if (audio_abierto && millis() - t > 30000) { t = millis(); estado(); }
    delay(10);
}
