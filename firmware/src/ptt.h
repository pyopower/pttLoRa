/* ptt.h — EL PTT DE LA PROPIA PLACA, separado de dónde viene.
 *
 * Sólo con -DAUDIO_LOCAL. Un transceptor tiene varios sitios desde los que se
 * puede pulsar, y ninguno debería saber de los demás:
 *
 *   - el BOTÓN DE FÁBRICA de la placa (LoRa32 GPIO 0 «PRG», T-Beam GPIO 38),
 *     que es el PTT de quien use auriculares Bluetooth normales: sus botones no
 *     sirven para esto (visto con el JBL);
 *   - un PULSADOR EXTERNO a masa (`AUDIO_PIN_PTT`, con pull-up interno);
 *   - los botones del MICRO BLUETOOTH (volumen, asistente de voz, o la orden AT
 *     propia de los micros para POC);
 *   - la CONSOLA, para probar sin tocar nada.
 *
 * Todos acaban en el mismo estado, que es lo único que mira la tarea de audio.
 * Lo que NO está aquí es el arbitraje ni el TOT: eso lo pone el nodo, igual que
 * para la app (ver `vigila_ptt` en main.cpp). Cuando el nodo corta, se avisa con
 * `ptt_suelta()` y el estado vuelve a reposo.
 *
 * DOS FORMAS DE PULSAR
 * --------------------
 *   - PULSADOR: abajo mientras se mantiene. Es lo natural en un botón físico.
 *   - CONMUTADOR: una pulsación abre y la siguiente cierra. Es lo que hacen los
 *     micros Bluetooth (un evento por pulsación, no dos), y para el botón de la
 *     placa es cuestión de gusto: se elige con `ptt_modo_boton()`.
 *
 * Con el conmutador, un botón mal dado deja el micro abierto: por eso el TOT
 * del nodo no es opcional, y al cortar queda dicho en la pantalla.
 */
#pragma once
#include <Arduino.h>

enum : uint8_t { PTT_O_NADIE = 0, PTT_O_PLACA, PTT_O_PIN, PTT_O_BT, PTT_O_CONSOLA };
enum : uint8_t { BOTON_SOLO_PANTALLA = 0, BOTON_PULSADOR, BOTON_CONMUTADOR };

void ptt_arranca(int pin_placa, int pin_externo, uint8_t modo_boton);
void ptt_atiende();                         // desde loop(): lee los botones físicos

void ptt_pulsa(uint8_t origen, bool abajo); // flanco de un pulsador (o del micro)
void ptt_conmuta(uint8_t origen);           // una pulsación de conmutador
void ptt_suelta(const char *motivo);        // el nodo cortó (TOT, canal ocupado...)

bool        ptt_activo();
uint8_t     ptt_origen();
const char *ptt_nombre_origen(uint8_t o);
const char *ptt_ultimo_corte();             // por qué se cortó la última vez ("" = nada)
void        ptt_olvida_corte();

void    ptt_modo_boton(uint8_t m);
uint8_t ptt_modo_boton();
