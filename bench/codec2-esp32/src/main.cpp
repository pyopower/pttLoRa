/*
 * BANCO DE PRUEBA: Codec2 dentro del ESP32.
 *
 * De aquí sale el número que decide si el nodo puede llevar micrófono propio
 * —por Bluetooth o por cable— y prescindir del móvil. Hoy el códec vive en los
 * extremos A PROPÓSITO (móvil y gateway), y meterlo en la placa es un cambio de
 * arquitectura que no se hace por intuición.
 *
 * Qué se mide, por cada modo que usa la app:
 *   - microsegundos por trama al CODIFICAR y al DESCODIFICAR,
 *   - qué fracción del tiempo real es eso (una trama de Codec2 son 20 o 40 ms
 *     de audio: si codificar 40 ms de voz cuesta 40 ms de CPU, no hay nada que
 *     hacer),
 *   - la RAM que se queda el codificador y el descodificador a la vez, que es
 *     como funcionaría de verdad un nodo autónomo (habla y escucha).
 *
 * Se alimenta con **voz real** (`voz.h`, un segundo de voz sintetica de Piper) y
 * no con un tono: el cuantificador de Codec2 busca en un libro de códigos, y
 * un seno es el caso fácil. Medir con un seno daría un número bonito y falso.
 */
#include <Arduino.h>
#include "codec2.h"
#include "voz.h"

/* Con `__EMBEDDED__` —que es lo que manda los libros de códigos a flash—
 * Codec2 deja de usar malloc directamente y pide que se los demos nosotros.
 * Aquí van al heap normal; en el nodo de verdad sería el sitio para elegir
 * memoria interna y no PSRAM, que para esto sería un freno. */
extern "C" {
void *codec2_malloc(size_t t)             { return malloc(t); }
void *codec2_calloc(size_t n, size_t t)   { return calloc(n, t); }
void  codec2_free(void *p)                { free(p); }
}

/** Pila de la tarea que mide. Generosa a propósito: aquí se trata de saber
 *  cuánta hace falta, no de apurarla. */
static const uint32_t PILA = 40 * 1024;

struct Modo { int id; const char *nombre; };
static const Modo MODOS[] = {
#ifdef SOLO_1200
    { CODEC2_MODE_1200, "1200" },
#else
    { CODEC2_MODE_3200, "3200" },
    { CODEC2_MODE_2400, "2400" },
    { CODEC2_MODE_1600, "1600" },
    { CODEC2_MODE_1300, "1300" },
    { CODEC2_MODE_1200, "1200" },
    { CODEC2_MODE_700C, "700C" },
#endif
};

static void mide(const Modo &m) {
    uint32_t heap0 = ESP.getFreeHeap();

    /* Los DOS a la vez, codificador y descodificador: un nodo autónomo habla y
       escucha, y la RAM que importa es la de los dos juntos. */
    struct CODEC2 *enc = codec2_create(m.id);
    struct CODEC2 *dec = codec2_create(m.id);
    if (!enc || !dec) {
        Serial.printf("%-5s  NO CABE: codec2_create devolvió nulo (heap %u)\n",
                      m.nombre, (unsigned)ESP.getFreeHeap());
        if (enc) codec2_destroy(enc);
        if (dec) codec2_destroy(dec);
        return;
    }
    uint32_t heapUsado = heap0 - ESP.getFreeHeap();

    int nMuestras = codec2_samples_per_frame(enc);
    int nBytes    = codec2_bytes_per_frame(enc);
    int msTrama   = (nMuestras * 1000) / 8000;

    static unsigned char bits[64];
    static short pcm[1024];

    int tramas = VOZ_N / nMuestras;
    if (tramas < 1) tramas = 1;

    /* Una pasada en vacío antes de cronometrar: la primera trama paga la caché
       de instrucciones fría (el código vive en flash y se mapea por caché), y
       contarla ensucia la media. */
    codec2_encode(enc, bits, (short *)VOZ);
    codec2_decode(dec, pcm, bits);

    uint32_t tEnc = 0, tDec = 0;
    for (int i = 0; i < tramas; i++) {
        const short *entrada = (const short *)VOZ + i * nMuestras;
        uint32_t t0 = micros();
        codec2_encode(enc, bits, (short *)entrada);
        uint32_t t1 = micros();
        codec2_decode(dec, pcm, bits);
        uint32_t t2 = micros();
        tEnc += t1 - t0;
        tDec += t2 - t1;
    }

    float encUs = (float)tEnc / tramas;
    float decUs = (float)tDec / tramas;
    float presupuesto = msTrama * 1000.0f;        // µs de audio por trama
    Serial.printf("%-5s  %2d ms/trama %2d B  enc %6.0f us (%4.1f%%)  "
                  "dec %6.0f us (%4.1f%%)  juntos %4.1f%%  RAM %u B\n",
                  m.nombre, msTrama, nBytes,
                  encUs, 100.0f * encUs / presupuesto,
                  decUs, 100.0f * decUs / presupuesto,
                  100.0f * (encUs + decUs) / presupuesto,
                  (unsigned)heapUsado);

    codec2_destroy(enc);
    codec2_destroy(dec);
}

/* ⚠️ EN SU PROPIA TAREA, Y CON PILA DE SOBRA. El primer intento midió desde
 * `setup()` y la placa se reiniciaba en bucle: «A stack overflow in task
 * loopTask». Arduino le da 8 KB de pila al hilo del sketch y Codec2 se los
 * pasa — el codificador trabaja con vectores en la PILA, no en el heap. Eso no
 * es un estorbo del banco: **es parte de la respuesta**, porque un nodo que
 * codifique tendrá que darle a esa tarea una pila grande. Por eso se mide
 * también cuánta pila llegó a gastar de verdad (`HighWaterMark`). */
static void tarea(void *) {
    Serial.println();
    Serial.println("=== Codec2 en el ESP32 — banco de PTT LoRa ===");
    Serial.printf("CPU %u MHz · heap libre %u B · flash de programa %u B\n",
                  (unsigned)getCpuFrequencyMhz(), (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getSketchSize());
    Serial.println("El % es sobre el tiempo real: 100% = el ESP32 tarda en");
    Serial.println("procesar una trama lo mismo que dura esa trama de audio.");
    Serial.println();
    for (unsigned i = 0; i < sizeof(MODOS) / sizeof(MODOS[0]); i++) {
        mide(MODOS[i]);
        delay(50);
    }
    unsigned libre = uxTaskGetStackHighWaterMark(NULL);
    Serial.println();
    Serial.printf("PILA: de los %u B dados quedaron %u B sin tocar "
                  "-> hacen falta ~%u B\n", (unsigned)PILA, libre,
                  (unsigned)PILA - libre);
    Serial.println("=== FIN ===");
    vTaskDelete(NULL);
}

void setup() {
    Serial.begin(115200);
    delay(1500);
    xTaskCreate(tarea, "banco", PILA, NULL, 5, NULL);
}

void loop() { delay(1000); }

#if 0
void setupViejo() {
    Serial.println();
    Serial.println("=== Codec2 en el ESP32 — banco de PTT LoRa ===");
    Serial.printf("CPU %u MHz · heap libre %u B · flash de programa %u B\n",
                  (unsigned)getCpuFrequencyMhz(), (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getSketchSize());
    Serial.println("El % es sobre el tiempo real: 100% = el ESP32 tarda en");
    Serial.println("procesar una trama lo mismo que dura esa trama de audio.");
    Serial.println();
    for (unsigned i = 0; i < sizeof(MODOS) / sizeof(MODOS[0]); i++) {
        mide(MODOS[i]);
        delay(50);
    }
    Serial.println();
    Serial.println("=== FIN ===");
}
#endif
