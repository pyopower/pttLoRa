/* audio_local.cpp — ver audio_local.h. */
#ifdef AUDIO_LOCAL

#include "audio_local.h"
#include "protocolo.h"
#include "codec2.h"
#include "audio_voz.h"
#include "audio_baliza.h"

/* Con `__EMBEDDED__` —lo que manda los libros de códigos a flash— Codec2 pide
   que le demos nosotros la memoria. Heap normal: la LoRa32 no tiene PSRAM, y en
   una que la tuviera sería más lenta para esto. */
extern "C" {
void *codec2_malloc(size_t t)             { return malloc(t); }
void *codec2_calloc(size_t n, size_t t)   { return calloc(n, t); }
void  codec2_free(void *p)                { free(p); }
}

void (*audio_log)(const char *) = nullptr;

static void dice(const char *fmt, ...)
{
    char s[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    if (audio_log) audio_log(s);
}

// --------------------------------------------------------------- modos ----
/* El código que VIAJA (C2_*, igual que la app) no es el de codec2.h: los de la
   librería no son consecutivos. Confundirlos no da error, abre otro códec y el
   audio sale irreconocible (ver Codec2.kt). */
static const int NATIVO[] = { CODEC2_MODE_3200, CODEC2_MODE_2400, CODEC2_MODE_1600,
                              CODEC2_MODE_1300, CODEC2_MODE_1200, CODEC2_MODE_700C };
static const char *NOMBRE[] = { "3200", "2400", "1600", "1300", "1200", "700C" };
#define N_MODOS 6

/* Lo que habla la placa. El 1200 es el de la app por defecto, y el medido. */
static const uint8_t MODO_TX = C2_1200;
static const uint32_t LOTE_MS = 480;            // como la app: lo que va en un paquete

/* Pila: Codec2 trabaja con vectores en la PILA. Medido con el firmware entero
   (15-sep): de 28 KB quedaron 10,4 KB sin tocar, o sea ~17,6 KB de uso. 22 KB
   dejan margen y le devuelven 6 KB al heap, que es lo que de verdad escasea. */
static const uint32_t PILA = 22 * 1024;
#ifndef AUDIO_PRIORIDAD
#define AUDIO_PRIORIDAD 1
#endif

// -------------------------------------------------------------- colas ----
struct AudioRx {
    uint8_t tipo;                   // AUD_*
    uint8_t len;
    uint8_t datos[AUDIO_LOTE_MAX + 2];
};

static QueueHandle_t q_tx = nullptr;   // audio -> loop()
static QueueHandle_t q_rx = nullptr;   // loop() -> audio
static TaskHandle_t  tarea = nullptr;

// ------------------------------------------------------------- estado ----
enum : uint8_t { PIDE_NADA = 0, PIDE_PRUEBA, PIDE_BANCO };
/* BALIZA CICLICA: la placa habla sola cada `periodo` segundos, codificando ella.
   Se apaga sola a los `minutos`: una baliza de prueba olvidada ocuparia el
   canal para siempre. */
static volatile uint32_t baliza_periodo_ms = 0, baliza_fin = 0, baliza_prox = 0;
static uint32_t baliza_n = 0;
static volatile uint8_t  pedido = PIDE_NADA;
static volatile uint8_t  pedido_veces = 1;
static volatile uint16_t pedido_seg = 20;
static volatile bool     parar = false;
static volatile bool     denegado = false;
static volatile bool     ocupada = false;       // hablando o midiendo
static int               pin_ptt = -1;

struct Medida {
    uint32_t n = 0, suma = 0, maximo = 0;
    void mete(uint32_t us) { n++; suma += us; if (us > maximo) maximo = us; }
    uint32_t media() const { return n ? suma / n : 0; }
    void reset() { n = suma = maximo = 0; }
};
static Medida m_enc, m_dec;
static uint32_t lotes_tx = 0, lotes_rx = 0, tramas_rx = 0, rx_perdidas = 0;
static uint32_t rx_modo_malo = 0, tarde = 0;
static uint32_t heap_min = UINT32_MAX;
static uint64_t rx_energia = 0;
static uint32_t rx_muestras = 0;

/* El latido de loop(): la vuelta más larga dice si el códec le está robando
   tiempo a la radio. Lo escribe loop() y lo lee el banco. */
static volatile uint32_t loop_t_ultimo = 0, loop_max_us = 0, loop_vueltas = 0;

void audio_latido_loop()
{
    uint32_t ahora = micros();
    if (loop_t_ultimo) {
        uint32_t d = ahora - loop_t_ultimo;
        if (d > loop_max_us) loop_max_us = d;
    }
    loop_t_ultimo = ahora;
    loop_vueltas++;
}

static void apunta_heap()
{
    uint32_t h = ESP.getFreeHeap();
    if (h < heap_min) heap_min = h;
}

// ---------------------------------------------------------- las fuentes --
/* Una fuente entrega tramas de 8 kHz y dice si sigue "con el PTT pulsado".
   La de PRUEBA es la voz grabada, `veces` pasadas seguidas. */
struct Fuente {
    virtual bool activa() = 0;
    virtual void lee(short *pcm, int n) = 0;
    virtual ~Fuente() {}
};

struct FuentePCM : Fuente {
    const short *pcm_;
    uint32_t n_, pos = 0, total;
    FuentePCM(const short *p, uint32_t n, uint8_t veces) : pcm_(p), n_(n), total(n * veces) {}
    bool activa() override { return pos < total && !parar; }
    void lee(short *pcm, int n) override {
        for (int i = 0; i < n; i++, pos++) pcm[i] = pos < total ? pcm_[pos % n_] : 0;
    }
};

/* PTT de un pulsador (o del adaptador del micro, que cierra a masa). La voz
   todavía no: hasta que haya ADC o I2S, con el PTT pulsado se manda silencio,
   que sirve para probar el cableado y el arbitraje. */
struct FuentePulsador : Fuente {
    uint32_t t_suelto = 0;
    bool activa() override {
        if (parar) return false;
        if (digitalRead(pin_ptt) == LOW) { t_suelto = 0; return true; }
        if (!t_suelto) t_suelto = millis();
        return millis() - t_suelto < 40;              // antirrebote al soltar
    }
    void lee(short *pcm, int n) override { memset(pcm, 0, n * sizeof(short)); }
};

// -------------------------------------------------------- el sumidero ----
/* NULO: no suena, pero cuenta energía para saber que llega voz y no ceros. Aquí
   irá el DAC o el I2S. */
static void sumidero(const short *pcm, int n)
{
    for (int i = 0; i < n; i++) rx_energia += (int32_t)pcm[i] * pcm[i];
    rx_muestras += n;
}

// ------------------------------------------------------------- hablar ----
static bool manda_tx(uint8_t tipo, const uint8_t *d = nullptr, uint8_t n_tramas = 0, uint8_t len = 0)
{
    AudioTx t;
    t.tipo = tipo;
    t.modo = MODO_TX;
    t.n = n_tramas;
    t.len = len;
    if (len) memcpy(t.datos, d, len);
    /* Si loop() no da abasto, se espera como mucho un lote: un lote que no sale
       a tiempo ya no sirve para nada. */
    return xQueueSend(q_tx, &t, pdMS_TO_TICKS(LOTE_MS)) == pdTRUE;
}

static struct CODEC2 *dec = nullptr;
static int8_t dec_modo = -1;

static void habla(Fuente &f, const char *que)
{
    /* Simplex: mientras se habla no se escucha. Soltar el descodificador son
       ~30 KB de heap que el codificador agradece (el banco vio 28 KB libres con
       los dos vivos). */
    if (dec) { codec2_destroy(dec); dec = nullptr; dec_modo = -1; }
    struct CODEC2 *enc = codec2_create(NATIVO[MODO_TX]);
    if (!enc) { dice("audio: no hay memoria para el codificador (heap %u)", ESP.getFreeHeap()); return; }
    codec2_set_natural_or_gray(enc, 1);
    apunta_heap();
    const int muestras = codec2_samples_per_frame(enc);
    const int bytes = codec2_bytes_per_frame(enc);
    const int por_lote = (LOTE_MS * 8) / muestras;
    const uint32_t trama_ms = muestras / 8;
    static short pcm[320];
    static uint8_t lote[AUDIO_LOTE_MAX];
    int en_lote = 0;
    uint32_t tramas = 0;

    denegado = false;
    manda_tx(AUD_INICIO);
    dice("audio: habla (%s), Codec2 %s", que, NOMBRE[MODO_TX]);
    TickType_t marca = xTaskGetTickCount();
    while (f.activa() && !denegado) {
        f.lee(pcm, muestras);
        uint32_t t0 = micros();
        codec2_encode(enc, lote + en_lote * bytes, pcm);
        m_enc.mete(micros() - t0);
        en_lote++;
        tramas++;
        if (en_lote == por_lote) {
            if (manda_tx(AUD_VOZ, lote, en_lote, en_lote * bytes)) lotes_tx++;
            en_lote = 0;
        }
        /* Al ritmo del tiempo real, como un micrófono: con la fuente de prueba
           esto es lo que evita escupir 3 s de voz en 200 ms. */
        if (xTaskDelayUntil(&marca, pdMS_TO_TICKS(trama_ms)) == pdFALSE) {
            tarde++;
            vTaskDelay(1);     // ir tarde no da permiso para no soltar la CPU
        }
    }
    if (en_lote && !denegado && manda_tx(AUD_VOZ, lote, en_lote, en_lote * bytes)) lotes_tx++;
    manda_tx(AUD_FIN);
    codec2_destroy(enc);
    dice("audio: fin (%lu tramas, %lu lotes, codificar %lu us de media, %lu max)%s",
         (unsigned long)tramas, (unsigned long)lotes_tx, (unsigned long)m_enc.media(),
         (unsigned long)m_enc.maximo, denegado ? " — el nodo no dio el turno" : "");
}

void audio_ptt_denegado(const char *motivo)
{
    denegado = true;
    dice("audio: sin turno: %s", motivo);
}

// ------------------------------------------------------------ escuchar ----

static void escucha(const AudioRx &r)
{
    if (r.tipo == AUD_INICIO) {
        uint8_t modo = r.len ? r.datos[0] : C2_1200;
        if (modo >= N_MODOS) { rx_modo_malo++; return; }
        if (dec_modo != (int8_t)modo) {
            if (dec) codec2_destroy(dec);
            dec = codec2_create(NATIVO[modo]);
            dec_modo = dec ? modo : -1;
            if (dec) codec2_set_natural_or_gray(dec, 1);
            else rx_modo_malo++;
            apunta_heap();
        }
        return;
    }
    if (r.tipo != AUD_VOZ || r.len < 2) return;
    uint8_t modo = r.datos[0], n = r.datos[1];
    if (modo >= N_MODOS) { rx_modo_malo++; return; }
    if (dec_modo != (int8_t)modo) {                  // llegó a mitad: se abre aquí
        AudioRx ini{AUD_INICIO, 1, {modo}};
        escucha(ini);
        if (!dec) return;
    }
    const int bytes = codec2_bytes_per_frame(dec);
    const int muestras = codec2_samples_per_frame(dec);
    static short pcm[320];
    for (int i = 0; i < n && 2 + (i + 1) * bytes <= r.len; i++) {
        uint32_t t0 = micros();
        codec2_decode(dec, pcm, (unsigned char *)r.datos + 2 + i * bytes);
        m_dec.mete(micros() - t0);
        sumidero(pcm, muestras);
        tramas_rx++;
    }
    lotes_rx++;
}

void audio_rx(uint8_t tipo, const uint8_t *cuerpo, uint8_t n, uint32_t, uint8_t)
{
    if (!q_rx) return;
    AudioRx r;
    r.tipo = tipo == T_INICIO ? AUD_INICIO : tipo == T_VOZ ? AUD_VOZ : AUD_FIN;
    r.len = n > sizeof r.datos ? sizeof r.datos : n;
    memcpy(r.datos, cuerpo, r.len);
    /* Nunca esperar desde loop(): si la tarea va atrasada, se pierde el lote y se
       cuenta. Frenar loop() es frenar la radio. */
    if (xQueueSend(q_rx, &r, 0) != pdTRUE) rx_perdidas++;
}

// --------------------------------------------------------------- banco ----
/* El número que faltaba: Codec2 con LoRa, WiFi, enlace y todo lo demás en
   marcha. Codifica la voz grabada y descodifica lo codificado al ritmo del
   tiempo real, SIN EMITIR, y mientras tanto mira cuánto se estira la vuelta
   de loop() —que es donde vive la radio—. */
struct Fase {
    Medida m;
    uint32_t tarde = 0, tramas = 0, loop_max = 0, vueltas_s = 0;
};

/* Una fase: codificar la voz grabada, o descodificar lo ya codificado, al ritmo
   del tiempo real y SIN EMITIR. Mientras, se mira cuánto se estira loop(). */
static void fase(bool codificar, uint16_t segundos, Fase &r, struct CODEC2 *c,
                 const uint8_t *bits_voz, int n_bits_voz)
{
    const int muestras = codec2_samples_per_frame(c);
    const int bytes = codec2_bytes_per_frame(c);
    const uint32_t trama_ms = muestras / 8;
    static short pcm[320];
    static uint8_t bits[16];
    uint32_t pos = 0, k = 0;
    loop_max_us = 0;
    uint32_t v0 = loop_vueltas;
    uint32_t t_fin = millis() + segundos * 1000UL;
    TickType_t marca = xTaskGetTickCount();
    while (millis() < t_fin && !parar) {
        uint32_t t0, t1;
        if (codificar) {
            for (int i = 0; i < muestras; i++, pos++) pcm[i] = VOZ[pos % VOZ_N];
            t0 = micros();
            codec2_encode(c, bits, pcm);
            t1 = micros();
        } else {
            t0 = micros();
            codec2_decode(c, pcm, (unsigned char *)bits_voz + (k++ % n_bits_voz) * bytes);
            t1 = micros();
        }
        r.m.mete(t1 - t0);
        r.tramas++;
        apunta_heap();
        if (xTaskDelayUntil(&marca, pdMS_TO_TICKS(trama_ms)) == pdFALSE) {
            r.tarde++;
            vTaskDelay(1);
        }
    }
    r.loop_max = loop_max_us;
    r.vueltas_s = (loop_vueltas - v0) / (segundos ? segundos : 1);
}

/* El número que faltaba: Codec2 con LoRa, WiFi, enlace y todo lo demás en
   marcha. SIMPLEX, como el transceptor de verdad: una fase codificando (hablar)
   y otra descodificando (escuchar), nunca las dos a la vez — medirlas juntas
   da el 85 % de un núcleo, que es un caso que no existe. */
static void banco(uint16_t segundos)
{
    uint16_t mitad = segundos / 2 ? segundos / 2 : 1;
    const float presupuesto = 40000.0f;
    heap_min = UINT32_MAX;

    /* Referencia de loop() sin códec, con el mismo tiempo. */
    loop_max_us = 0;
    uint32_t v0 = loop_vueltas;
    vTaskDelay(pdMS_TO_TICKS(mitad * 1000UL));
    uint32_t ref_max = loop_max_us, ref_v = (loop_vueltas - v0) / mitad;

    struct CODEC2 *e = codec2_create(NATIVO[MODO_TX]);
    if (!e) { dice("banco: sin memoria para codificar (heap %u)", ESP.getFreeHeap()); return; }
    /* Voz codificada para la fase de escuchar: un segundo, sacado con el mismo
       codificador antes de empezar. */
    const int bytes = codec2_bytes_per_frame(e), muestras = codec2_samples_per_frame(e);
    const int n_bits = VOZ_N / muestras;
    uint8_t *bits_voz = (uint8_t *)malloc(n_bits * bytes);
    if (!bits_voz) { codec2_destroy(e); dice("banco: sin memoria"); return; }
    for (int i = 0; i < n_bits; i++)
        codec2_encode(e, bits_voz + i * bytes, (short *)VOZ + i * muestras);

    Fase fe, fd;
    fase(true, mitad, fe, e, nullptr, 0);
    uint32_t heap_enc = heap_min;
    codec2_destroy(e);

    struct CODEC2 *d = codec2_create(NATIVO[MODO_TX]);
    if (!d) { free(bits_voz); dice("banco: sin memoria para descodificar"); return; }
    heap_min = UINT32_MAX;
    fase(false, mitad, fd, d, bits_voz, n_bits);
    codec2_destroy(d);
    free(bits_voz);

    dice("banco nucleo %d, Codec2 %s, %us por fase. HABLAR: %lu us (%.1f%%) max %lu, "
         "tarde %lu/%lu, loop() %lu vueltas/s y vuelta max %lu us, heap min %u",
         xPortGetCoreID(), NOMBRE[MODO_TX], mitad,
         (unsigned long)fe.m.media(), 100.0f * fe.m.media() / presupuesto, (unsigned long)fe.m.maximo,
         (unsigned long)fe.tarde, (unsigned long)fe.tramas,
         (unsigned long)fe.vueltas_s, (unsigned long)fe.loop_max, (unsigned)heap_enc);
    dice("banco: ESCUCHAR: %lu us (%.1f%%) max %lu, tarde %lu/%lu, loop() %lu vueltas/s "
         "y vuelta max %lu us, heap min %u · SIN CODEC: loop() %lu vueltas/s, max %lu us · pila libre %u B",
         (unsigned long)fd.m.media(), 100.0f * fd.m.media() / presupuesto, (unsigned long)fd.m.maximo,
         (unsigned long)fd.tarde, (unsigned long)fd.tramas,
         (unsigned long)fd.vueltas_s, (unsigned long)fd.loop_max, (unsigned)heap_min,
         (unsigned long)ref_v, (unsigned long)ref_max, (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

// --------------------------------------------------------------- tarea ----
static void bucle(void *)
{
    for (;;) {
        ocupada = true;
        if (baliza_periodo_ms && millis() >= baliza_fin) {
            baliza_periodo_ms = 0;
            dice("baliza: apagada sola (%lu emisiones)", (unsigned long)baliza_n);
        }
        if (pedido == PIDE_PRUEBA) {
            pedido = PIDE_NADA;
            parar = false;
            FuentePCM f(BALIZA, BALIZA_N, pedido_veces);
            habla(f, "baliza grabada");
        } else if (baliza_periodo_ms && millis() >= baliza_prox) {
            baliza_prox = millis() + baliza_periodo_ms;
            parar = false;
            baliza_n++;
            FuentePCM f(BALIZA, BALIZA_N, 1);
            char que[40];
            snprintf(que, sizeof que, "baliza ciclica n.%lu", (unsigned long)baliza_n);
            habla(f, que);
        } else if (pin_ptt >= 0 && digitalRead(pin_ptt) == LOW) {
            parar = false;
            FuentePulsador f;
            habla(f, "pulsador");
        }
        ocupada = false;
        AudioRx r;
        if (xQueueReceive(q_rx, &r, pdMS_TO_TICKS(20)) == pdTRUE) escucha(r);
    }
}

static void crea_tarea(int8_t nucleo)
{
    /* Prioridad 1, la misma que loop(): con 2 el audio ganaba siempre y la
       radio se quedaba con las sobras (medido: de 2.985 a 289 vueltas/s). */
    xTaskCreatePinnedToCore(bucle, "audio", PILA, nullptr, AUDIO_PRIORIDAD, &tarea,
                            nucleo < 0 ? 1 : nucleo);
}

void audio_arranca(int pin)
{
    pin_ptt = pin;
    if (pin_ptt >= 0) pinMode(pin_ptt, INPUT_PULLUP);
    q_tx = xQueueCreate(6, sizeof(AudioTx));
    q_rx = xQueueCreate(6, sizeof(AudioRx));
    crea_tarea(AUDIO_NUCLEO);
    apunta_heap();
}

bool audio_saca_tx(AudioTx *t)
{
    return q_tx && xQueueReceive(q_tx, t, 0) == pdTRUE;
}

void audio_prueba(uint8_t veces)
{
    pedido_veces = veces ? veces : 1;
    pedido = PIDE_PRUEBA;
}

/* El banco va en SU PROPIA tarea, clavada al núcleo que se pida, y se borra
   sola al acabar: así se mide en cualquier núcleo sin tocar la tarea de audio. */
static volatile bool en_banco = false;

static void tarea_banco(void *arg)
{
    banco((uint16_t)(uintptr_t)arg);
    en_banco = false;
    vTaskDelete(NULL);
}

void audio_banco(uint16_t segundos, int8_t nucleo)
{
    if (en_banco || ocupada) { dice("banco: ya hay algo en marcha"); return; }
    en_banco = true;
    parar = false;
    if (xTaskCreatePinnedToCore(tarea_banco, "banco", PILA, (void *)(uintptr_t)(segundos ? segundos : 20),
                                AUDIO_PRIORIDAD, nullptr,
                                nucleo < 0 ? (tarea ? xTaskGetAffinity(tarea) : 1) : nucleo) != pdPASS) {
        en_banco = false;
        dice("banco: no hay memoria para la tarea (heap %u)", ESP.getFreeHeap());
    }
}

void audio_baliza(uint16_t periodo_s, uint16_t minutos)
{
    if (!periodo_s) {
        baliza_periodo_ms = 0;
        dice("baliza: apagada (%lu emisiones)", (unsigned long)baliza_n);
        return;
    }
    if (periodo_s < 15) periodo_s = 15;          // el canal es de todos
    if (!minutos || minutos > 120) minutos = 30;
    baliza_n = 0;
    baliza_fin = millis() + minutos * 60000UL;
    baliza_prox = millis() + 1000;
    baliza_periodo_ms = periodo_s * 1000UL;
    dice("baliza: cada %u s durante %u min (%.1f s de voz, Codec2 1200)",
         periodo_s, minutos, BALIZA_N / 8000.0f);
}

void audio_para()
{
    baliza_periodo_ms = 0;
    parar = true;
}

void audio_estado(char *s, size_t cap)
{
    uint32_t rms = rx_muestras ? (uint32_t)sqrt((double)rx_energia / rx_muestras) : 0;
    snprintf(s, cap,
             "audio: nucleo=%d ptt_pin=%d tx=%lu lotes enc=%lu/%luus rx=%lu lotes %lu tramas "
             "dec=%lu/%luus rms=%lu perdidas=%lu modo_malo=%lu tarde=%lu heap_min=%u pila=%u "
             "loop_max=%luus",
             tarea ? (int)xTaskGetAffinity(tarea) : -1, pin_ptt,
             (unsigned long)lotes_tx, (unsigned long)m_enc.media(), (unsigned long)m_enc.maximo,
             (unsigned long)lotes_rx, (unsigned long)tramas_rx,
             (unsigned long)m_dec.media(), (unsigned long)m_dec.maximo, (unsigned long)rms,
             (unsigned long)rx_perdidas, (unsigned long)rx_modo_malo, (unsigned long)tarde,
             (unsigned)heap_min, tarea ? (unsigned)uxTaskGetStackHighWaterMark(tarea) : 0,
             (unsigned long)loop_max_us);
}

#endif  // AUDIO_LOCAL
