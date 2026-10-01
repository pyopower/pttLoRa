/* audio_bt.h — el micro-altavoz Bluetooth del transceptor (manos libres, HFP-AG).
 *
 * Sólo con -DAUDIO_BT (entorno `transceptor`), que además exige Arduino como
 * componente de ESP-IDF: el core precompilado trae el Bluetooth en papel de
 * AURICULAR y sin canal de audio SCO. Ver sdkconfig.defaults.
 *
 * La placa hace de «teléfono» (Audio Gateway) y el micro-altavoz de manos
 * libres. El audio va por SCO, CVSD a 8 kHz, que es justo lo que come Codec2
 * sin remuestrear. Aquí sólo hay Bluetooth y niveles; el códec, el ritmo y el
 * PTT son de `audio_local` y de `ptt`:
 *
 *   micro  ──SCO──► anillo ──► hfp_lee()      ──► audio_local (Codec2) ──► aire
 *   altavoz◄──SCO── anillo ◄── hfp_escribe()  ◄── audio_local (Codec2) ◄── aire
 *
 * Los botones del micro llegan como órdenes AT y se traducen a `ptt_pulsa()`.
 *
 * ⚠️ NO CONVIVE CON NimBLE: son dos pilas Bluetooth distintas sobre el mismo
 * controlador. El entorno `transceptor` compila sin BLE (-DSIN_BLE), así que no
 * hay app por Bluetooth; se configura por USB.
 */
#pragma once
#include <Arduino.h>

/* En qué paso está el micro. Es lo que enseña la pantalla: cuando algo no
   suena, lo que hay que saber es DÓNDE se quedó. */
enum : uint8_t { HFP_SIN_VINCULAR = 0, HFP_BUSCANDO, HFP_SIN_AUDIO, HFP_LISTO, HFP_SIN_PILA };

bool hfp_arranca();                             // desde setup()
void hfp_atiende();                             // desde loop(): reconexión, NVS, avisos

/* Audio. Lo llama la tarea de audio, nunca la pila Bluetooth. */
int  hfp_lee(short *pcm, int n, uint32_t espera_ms);   // muestras leídas (0 si no hay)
void hfp_vacia_micro();                                // tirar lo viejo al abrir el PTT
void hfp_escribe(const short *pcm, int n);             // ya con su ganancia
void hfp_agc(short *pcm, int n);                       // control de ganancia del micro
bool hfp_listo();                                      // hay audio en los dos sentidos

uint8_t     hfp_fase();
const char *hfp_nombre();                       // del micro, o "" si no se sabe

/* Órdenes. */
void hfp_buscar();
bool hfp_conecta(const uint8_t mac[6]);
void hfp_olvida();
void hfp_estado(char *s, size_t cap);
