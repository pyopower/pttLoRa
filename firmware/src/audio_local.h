/* audio_local.h — la placa habla y escucha SOLA, sin móvil.
 *
 * Sólo se compila con -DAUDIO_LOCAL (entornos `*-audio` de platformio.ini). El
 * firmware normal no lleva nada de esto: el códec sigue viviendo en los
 * extremos, que es lo que se decidió, y un nodo con micrófono propio es una
 * variante, no el caso general.
 *
 * QUÉ HACE Y QUÉ NO
 * -----------------
 * Aquí está TODO lo que es audio: la tarea que codifica y descodifica Codec2,
 * las fuentes (de dónde sale la voz) y los sumideros (a dónde va), y el PTT
 * físico. Lo que NO está aquí es la radio, los tubos ni el arbitraje: eso sigue
 * siendo de `main.cpp`, que no es seguro tocar desde otra tarea. La frontera son
 * dos colas:
 *
 *    tarea de audio  ──(INICIO / VOZ / FIN)──►  loop() → orden(..., T_LOCAL)
 *    tarea de audio  ◄──(lo que llega)────────  loop() → entrega_al_anfitrion
 *
 * `T_LOCAL` es un tubo más, como el de la app: así el micrófono de la placa
 * pasa por el MISMO arbitraje de PTT, el mismo TOT, el mismo eco local y el
 * mismo enlace que la voz del móvil, sin duplicar ni una regla.
 *
 * FUENTES Y SUMIDEROS
 * -------------------
 * Hoy hay una fuente de PRUEBA (un segundo de voz real grabada, en flash) y un
 * sumidero NULO (descodifica y mide, pero no suena). Son lo que hace falta para
 * medir con el firmware entero en marcha y para probar el camino completo por
 * el aire sin hardware de audio. Las de verdad —el adaptador del micro con
 * cable (ADC + DAC) o I2S— se enchufan en el mismo sitio sin tocar nada más.
 */
#pragma once
#include <Arduino.h>

#define AUDIO_LOTE_MAX   192        // bytes de códec de un lote (cabe en MAX_PAYLOAD - 2)

enum : uint8_t { AUD_INICIO = 1, AUD_VOZ, AUD_FIN };

/* Lo que la tarea de audio le pide a loop() que emita. */
struct AudioTx {
    uint8_t tipo;                   // AUD_*
    uint8_t modo;                   // código de modo como viaja (C2_*)
    uint8_t n;                      // tramas de códec en el lote
    uint8_t len;                    // bytes en `datos`
    uint8_t datos[AUDIO_LOTE_MAX];
};

/* Dónde escribe la tarea de audio lo que tiene que contar. Lo pone main.cpp. */
extern void (*audio_log)(const char *);

void audio_arranca(int pin_ptt);
bool audio_saca_tx(AudioTx *t);                         // desde loop()
void audio_ptt_denegado(const char *motivo);            // el nodo no le dio el turno
void audio_rx(uint8_t tipo, const uint8_t *cuerpo, uint8_t n, uint32_t src, uint8_t stream);
void audio_latido_loop();                               // una vez por vuelta de loop()

/* Órdenes (CMD_AUDIO). */
void audio_prueba(uint8_t veces);                       // hablar la voz grabada por el aire
void audio_banco(uint16_t segundos, int8_t nucleo);    // medir con todo en marcha, sin emitir
void audio_baliza(uint16_t periodo_s, uint16_t minutos);  // 0 = apagar
void audio_para();
void audio_estado(char *s, size_t cap);
