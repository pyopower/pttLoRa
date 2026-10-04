/* audio_bt.cpp — ver audio_bt.h. Sale del banco bench/hfp-ag, validado en el
   aire el 15-sep-2026 con un JBL GO. */
#if defined(AUDIO_LOCAL) && defined(AUDIO_BT)

#include "audio_bt.h"
#include "audio_local.h"
#include "ptt.h"
#include <Preferences.h>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_ag_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "freertos/ringbuf.h"
#include "esp_coexist.h"

/* ⚠️ Sin esto Arduino, al arrancar, LIBERA la memoria del Bluetooth
   (`initArduino` llama a esp_bt_controller_mem_release si btInUse() dice que
   no) y luego el controlador ya no se puede iniciar: ESP_ERR_INVALID_STATE.
   En C: en C++ no sustituye a la débil de Arduino. */
extern "C" bool btInUse() { return true; }

static Preferences prefs;
static bool pila_ok = false;
static esp_bd_addr_t par = {0};
static volatile bool hay_par = false;
static volatile bool slc = false, audio_abierto = false;
static char nombre_par[24] = "";

/* Lo que la pila Bluetooth descubre en SUS callbacks y hay que hacer en loop():
   escribir en la NVS desde la pila Bluetooth es buscarse problemas (con el SPP
   rompía la sesión en curso). */
static volatile bool guardar_par = false, guardar_nombre = false, guardar_propio = false;

/* ¿El micro trae PTT PROPIO? Se aprende solo: el primer AT+BLDN lo delata
   (ver `boton_rellamada`) y queda en la NVS con el micro. Mientras no lo trae,
   el volumen hace de PTT (el truco del JBL); en cuanto lo trae, el volumen
   vuelve a ser volumen —con el Abbree, sus +/− abrirían el canal—. */
static volatile bool ptt_propio = false;
/* Dónde está el botón FÍSICO según los BLDN contados: abajo/arriba. No es lo
   mismo que «se está emitiendo»: si el nodo corta por TOT con el PTT aún
   pulsado, el BLDN siguiente es el de SOLTAR y no debe volver a abrir. */
static volatile bool bldn_abajo = false;
static volatile uint32_t t_bldn = 0;
static volatile uint32_t reabrir_en = 0;

/* AVRCP, para el PTT del Abbree (KST_vHMIC010). Medido el 3-oct-2026 con el
   micro pegado a la placa: con el audio de manos libres abierto, que es como
   trabaja el transceptor, NINGUN boton del Abbree manda nada por HFP. Su PTT
   va por AVRCP —FAST FORWARD al pulsar (una rafaga: toque y medio segundo
   mantenido) y REWIND al soltar—, igual que con un movil. Y el Abbree solo abre
   AVRCP despues de A2DP, asi que la placa se registra tambien como FUENTE A2DP
   (nunca manda musica: el audio sigue por el canal de manos libres) y abre ese
   enlace en cuanto el micro esta enganchado. */
static volatile bool a2dp_ok = false, avrc_ok = false, avrc_abajo = false;
static volatile uint32_t abrir_a2dp_en = 0;
/* QUIEN abre A2DP importa (medido el 3-oct): si lo abre la placa tras el SLC,
   el Abbree lo mantiene; si lo abre EL (al reconectar ya conoce a la placa),
   lo cierra a los 4 s, se lleva el AVRCP —adios PTT— y el vaiven mete un
   zumbido en el altavoz. Asi que si llega abierto por el micro, se cierra y se
   vuelve a abrir desde aqui. */
static volatile bool a2dp_mio = false, rehacer_a2dp = false;

/* Volumen del altavoz que se le impone al micro tras cada pulsación: alto para
   oír bien y con margen para que el botón siga mandando eventos (en el tope de
   15 el «+» ya no manda nada y el PTT se quedaría pulsado). */
static const int VOLUMEN = 13;
static const float G_ALTAVOZ = 1.8f;

static uint32_t heap_antes = 0, heap_pila = 0;

/* EMPAREJAR SIN CONSOLA. La búsqueda apunta aquí los aparatos de AUDIO que
   ve (desde la tarea de la pila: por eso el cerrojo) y loop() decide al acabar.
   Con `emp_auto` (doble reset) se engancha solo al de más señal; si no aparece
   ninguno, se repite hasta tres veces (30 s) para dar tiempo a poner el micro
   en modo emparejar. */
struct Candidato { uint8_t mac[6]; int8_t rssi; char nombre[24]; };
static Candidato cand[8];
static volatile uint8_t n_cand = 0;
static portMUX_TYPE cerrojo_cand = portMUX_INITIALIZER_UNLOCKED;
static volatile bool buscando = false, fin_busqueda = false;
static bool emp_auto = false, emp_al_arrancar = false;
static uint8_t emp_intentos = 0;
/* Lo que enseña la pantalla: 0 nada · 1 buscando · 2 conectando · 3 no hay. */
static uint8_t emp_fase = 0;
static uint32_t emp_t = 0;

static void acaba_busqueda();

static void apunta_candidato(const uint8_t *mac, int rssi, const char *nombre)
{
    portENTER_CRITICAL(&cerrojo_cand);
    int i = 0;
    while (i < n_cand && memcmp(cand[i].mac, mac, 6)) i++;
    if (i == n_cand && n_cand < 8) {
        memcpy(cand[i].mac, mac, 6);
        cand[i].rssi = -127;
        cand[i].nombre[0] = 0;
        n_cand++;
    }
    if (i < n_cand) {
        if (rssi && rssi > cand[i].rssi) cand[i].rssi = rssi;
        if (nombre[0]) snprintf(cand[i].nombre, sizeof cand[i].nombre, "%s", nombre);
    }
    portEXIT_CRITICAL(&cerrojo_cand);
}

static void mac_txt(const uint8_t *b, char *s)
{
    snprintf(s, 18, "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
}

// ------------------------------------------------------------------ audio --
/* Dos anillos de bytes: lo que llega del micro y lo que va al altavoz. Los
   rellena y vacía la pila Bluetooth desde SUS tareas, así que aquí no se hace
   nada pesado. 3 KB = 192 ms de audio cada uno. */
static RingbufHandle_t rb_mic = nullptr, rb_alt = nullptr;
static volatile uint32_t bytes_mic = 0, bytes_alt = 0, huecos_alt = 0;
static volatile uint32_t mic_bloques = 0, mic_vacios = 0, mic_tirados = 0;
static volatile uint32_t n_pide = 0, n_listo = 0, n_pide_cero = 0;   // DIAGNOSTICO

static void llega_del_micro(const uint8_t *buf, uint32_t len)
{
    bytes_mic += len;
    /* Un bloque TODO a cero no es silencio de verdad (el micro siempre tiene
       algo de ruido): es un paquete SCO perdido que la pila ha rellenado. */
    mic_bloques++;
    bool cero = true;
    for (uint32_t i = 0; i < len && cero; i++) cero = buf[i] == 0;
    if (cero) mic_vacios++;
    if (rb_mic && xRingbufferSend(rb_mic, buf, len, 0) != pdTRUE) mic_tirados++;
}

static uint32_t pide_el_altavoz(uint8_t *buf, uint32_t len)
{
    /* SIEMPRE se entrega lo pedido: si no hay audio listo, silencio. Devolver 0
       o menos de lo pedido era lo que sonaba a chasquidos en reposo. */
    size_t puesto = 0;
    while (rb_alt && puesto < len) {
        size_t n = 0;
        uint8_t *d = (uint8_t *)xRingbufferReceiveUpTo(rb_alt, &n, 0, len - puesto);
        if (!d) break;
        memcpy(buf + puesto, d, n);
        vRingbufferReturnItem(rb_alt, d);
        puesto += n;
    }
    if (puesto < len) { huecos_alt++; memset(buf + puesto, 0, len - puesto); }
    bytes_alt += len;
    n_pide++;
    bool cero = true;
    for (uint32_t i = 0; i < len && cero; i++) cero = buf[i] == 0;
    if (cero) n_pide_cero++;
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
        if (hay >= 120) { esp_hf_outgoing_data_ready(); n_listo++; }
    }
}

int hfp_lee(short *pcm, int n, uint32_t espera_ms)
{
    if (!rb_mic) return 0;
    size_t quiero = n * sizeof(short), puesto = 0;
    TickType_t fin = xTaskGetTickCount() + pdMS_TO_TICKS(espera_ms);
    while (puesto < quiero) {
        TickType_t ahora = xTaskGetTickCount();
        TickType_t queda = fin > ahora ? fin - ahora : 0;
        size_t k = 0;
        uint8_t *d = (uint8_t *)xRingbufferReceiveUpTo(rb_mic, &k, queda, quiero - puesto);
        if (!d) break;
        memcpy((uint8_t *)pcm + puesto, d, k);
        vRingbufferReturnItem(rb_mic, d);
        puesto += k;
    }
    return puesto / sizeof(short);
}

void hfp_vacia_micro()
{
    if (!rb_mic) return;
    size_t k;
    void *d;
    while ((d = xRingbufferReceiveUpTo(rb_mic, &k, 0, 4096)) != nullptr) vRingbufferReturnItem(rb_mic, d);
}

/* Un limitador suave: recortar a lo bruto suena peor que comprimir un poco, y
   Codec2 a 1200 exagera tanto lo flojo como lo recortado. */
static inline short limita(float x)
{
    const float U = 24000.0f;                 // a partir de aquí, comprimir
    float a = x < 0 ? -x : x;
    if (a > U) a = U + (32767.0f - U) * (1.0f - expf(-(a - U) / (32767.0f - U)));
    return (short)(x < 0 ? -a : a);
}

void hfp_escribe(const short *pcm, int n)
{
    if (!rb_alt || !audio_abierto) return;
    static short sal[320];
    while (n > 0) {
        int k = n > 320 ? 320 : n;
        for (int i = 0; i < k; i++) sal[i] = limita(pcm[i] * G_ALTAVOZ);
        /* Si el altavoz no da abasto se tira: esperar aquí es retrasar todo lo
           que viene detrás. */
        xRingbufferSend(rb_alt, sal, k * sizeof(short), 0);
        pcm += k;
        n -= k;
    }
}

/* NIVELES DEL MICRO. El del JBL llega unos 30 dB por debajo de lo que Codec2
   necesita (RMS 60-200 hablando, frente a 2.000-5.000 de una voz normal). Así
   que CONTROL AUTOMÁTICO DE GANANCIA hacia RMS 3.000: baja rápido si se pasa
   (hablar pegado) y sube despacio; con silencio no sube, para no convertir el
   ruido de fondo en un soplido. */
static float g_mic = 8.0f;
static volatile uint32_t pico_mic = 0, rms_mic = 0;

void hfp_agc(short *pcm, int n)
{
    double e = 0;
    for (int i = 0; i < n; i++) e += (double)pcm[i] * pcm[i];
    float rms = sqrtf(e / n);
    for (int i = 0; i < n; i++) { uint32_t a = abs(pcm[i]); if (a > pico_mic) pico_mic = a; }
    /* PUERTA DE RUIDO: por debajo de esto no es voz, y subirlo sólo fabrica
       soplido. Se atenúa en vez de amplificar. */
    if (rms < 60) {
        for (int i = 0; i < n; i++) pcm[i] = pcm[i] / 4;
        rms_mic = rms / 4;
        return;
    }
    float objetivo = 3000.0f / rms;
    if (objetivo > 12.0f) objetivo = 12.0f;
    if (objetivo < 0.5f) objetivo = 0.5f;
    if (objetivo < g_mic) g_mic = objetivo;                    // bajar ya
    else g_mic += (objetivo - g_mic) * 0.05f;                  // subir poco a poco
    double e2 = 0;
    for (int i = 0; i < n; i++) { pcm[i] = limita(pcm[i] * g_mic); e2 += (double)pcm[i] * pcm[i]; }
    rms_mic = sqrtf(e2 / n);
}

bool hfp_listo() { return slc && audio_abierto; }

uint8_t hfp_fase()
{
    if (!pila_ok) return HFP_SIN_PILA;
    if (!slc) return hay_par ? HFP_BUSCANDO : HFP_SIN_VINCULAR;
    return audio_abierto ? HFP_LISTO : HFP_SIN_AUDIO;
}

const char *hfp_nombre() { return nombre_par; }

// ----------------------------------------------------------- manos libres --
static const char *EST_CON[] = { "desconectado", "conectando", "conectado", "SLC listo", "desconectando" };
static const char *EST_AUD[] = { "cerrado", "abriendo", "abierto (CVSD 8 kHz)", "abierto (mSBC 16 kHz)" };

/* LOS BOTONES DEL MICRO Y EL PTT. En manos libres no existe el «PTT»: cada
   botón llega como una orden AT. Lo medido con el JBL (15-sep):
     - «tel» sin llamada casi nunca manda nada → no sirve;
     - volumen llega SIEMPRE (+VGS): bajar = PTT abajo, subir = PTT arriba;
   y los micros hechos para POC (el Abbree) suelen traer una orden propia tipo
   AT+PTT=P / AT+PTT=R, que llega como «AT desconocida». Se apunta todo. */
static void boton_volumen(int v)
{
    static int antes = -1;
    if (ptt_propio) { antes = -1; return; }   // volumen de verdad: que lo lleve el micro
    if (antes >= 0 && v < antes) ptt_pulsa(PTT_O_BT, true);
    else if (antes >= 0 && v > antes) ptt_pulsa(PTT_O_BT, false);
    if (v != VOLUMEN) esp_bt_hf_volume_control(par, ESP_HF_VOLUME_CONTROL_TARGET_SPK, VOLUMEN);
    antes = VOLUMEN;
}

/* AT+BLDN («rellamada»). Medido con el Abbree (KST_vHMIC010, 2-oct-2026): sin
   A2DP/AVRCP, que es como lo ve esta placa, el PTT manda un AT+BLDN AL PULSAR
   y otro AL SOLTAR (corta: 0,26 s entre los dos; 36 s mantenido: el segundo
   llega al soltar, no por tiempo). Con un móvil el mismo PTT va por AVRCP
   (FF/REWIND) y lo que manda BLDN es el botón P1: si P1 también lo manda aquí,
   este conmutador lo tomará como PTT — se verá en el banco.
   Es un conmutador: se lleva la cuenta del botón (`bldn_abajo`) y no del
   estado de la emisión, para que un corte del nodo no lo desincronice. */
static void boton_rellamada()
{
    uint32_t ahora = millis();
    if (t_bldn && ahora - t_bldn < 120) return;   // rebote: un flanco, un BLDN
    t_bldn = ahora;
    if (!ptt_propio) {
        ptt_propio = true;
        guardar_propio = true;
        audio_dice("micro: trae PTT propio (AT+BLDN); el volumen deja de ser PTT");
    }
    bldn_abajo = !bldn_abajo;
    audio_dice("micro: PTT %s (AT+BLDN)", bldn_abajo ? "abajo" : "arriba");
    if (bldn_abajo) ptt_pulsa(PTT_O_BT, true);
    else if (ptt_origen() == PTT_O_BT) ptt_pulsa(PTT_O_BT, false);
}

/* La tecla del Abbree por AVRCP: FF = pulsar, REWIND = soltar. Solo cuenta
   el "pulsado" de cada una; la rafaga del FF trae dos y se ignora la segunda. */
static void tecla_avrcp(uint8_t key, uint8_t estado)
{
    if (estado != ESP_AVRC_PT_CMD_STATE_PRESSED) return;
    bool abajo;
    if (key == ESP_AVRC_PT_CMD_FAST_FORWARD) abajo = true;
    else if (key == ESP_AVRC_PT_CMD_REWIND) abajo = false;
    else { audio_dice("micro: tecla AVRCP 0x%02x", key); return; }
    if (!ptt_propio) {
        ptt_propio = true;
        guardar_propio = true;
        audio_dice("micro: trae PTT propio (AVRCP); el volumen deja de ser PTT");
    }
    if (abajo == avrc_abajo) return;
    avrc_abajo = abajo;
    audio_dice("micro: PTT %s (AVRCP)", abajo ? "abajo" : "arriba");
    if (abajo) ptt_pulsa(PTT_O_BT, true);
    else if (ptt_origen() == PTT_O_BT) ptt_pulsa(PTT_O_BT, false);
}

static void avrc_tg(esp_avrc_tg_cb_event_t ev, esp_avrc_tg_cb_param_t *p)
{
    switch (ev) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
        avrc_ok = p->conn_stat.connected;
        audio_dice("micro: AVRCP %s", avrc_ok ? "enlazado" : "cerrado");
        if (!avrc_ok && avrc_abajo) {             // se fue con el PTT pulsado
            avrc_abajo = false;
            if (ptt_origen() == PTT_O_BT) ptt_suelta("se fue el AVRCP del micro");
        }
        break;
    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
        tecla_avrcp(p->psth_cmd.key_code, p->psth_cmd.key_state);
        break;
    default: break;
    }
}

static void avrc_ct(esp_avrc_ct_cb_event_t, esp_avrc_ct_cb_param_t *) {}

static void a2dp(esp_a2d_cb_event_t ev, esp_a2d_cb_param_t *p)
{
    if (ev == ESP_A2D_CONNECTION_STATE_EVT) {
        a2dp_ok = p->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED;
        if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            audio_dice("micro: A2DP enlazado (%s)", a2dp_mio ? "lo abri yo" : "lo abrio el micro");
            if (!a2dp_mio) rehacer_a2dp = true;      // ver a2dp_mio
        } else if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            audio_dice("micro: A2DP cerrado (motivo %d)", (int)p->conn_stat.disc_rsn);
            a2dp_mio = false;
        }
    }
}

/* Nunca se arranca el flujo A2DP; si alguien lo pidiera, silencio. */
static int32_t a2dp_datos(uint8_t *d, int32_t len)
{
    if (d && len > 0) memset(d, 0, len);
    return len > 0 ? len : 0;
}

/* Una orden AT que no es del estándar. Si parece de PTT, se obedece. */
static void boton_desconocido(const char *at)
{
    audio_dice("micro: AT desconocida \"%s\"", at);
    String s(at);
    s.toUpperCase();
    if (s.indexOf("PTT") < 0) return;
    /* Las variantes vistas en micros para POC: =P/=R, =1/=0, =ON/=OFF,
       DOWN/UP. Si no se reconoce, se apunta y ya: con el banco se verá. */
    if (s.endsWith("=P") || s.endsWith("=1") || s.endsWith("ON") || s.indexOf("DOWN") >= 0 || s.indexOf("PRESS") >= 0)
        ptt_pulsa(PTT_O_BT, true);
    else if (s.endsWith("=R") || s.endsWith("=0") || s.endsWith("OFF") || s.indexOf("UP") >= 0 || s.indexOf("RELEASE") >= 0)
        ptt_pulsa(PTT_O_BT, false);
}

static void manos_libres(esp_hf_cb_event_t ev, esp_hf_cb_param_t *p)
{
    char m[18];
    switch (ev) {
    case ESP_HF_CONNECTION_STATE_EVT:
        mac_txt(p->conn_stat.remote_bda, m);
        audio_dice("micro: %s (%s)", p->conn_stat.state <= 4 ? EST_CON[p->conn_stat.state] : "?", m);
        slc = p->conn_stat.state == ESP_HF_CONNECTION_STATE_SLC_CONNECTED;
        audio_despierta();
        if (slc) {
            if (!hay_par || memcmp(par, p->conn_stat.remote_bda, 6)) guardar_par = true;
            memcpy(par, p->conn_stat.remote_bda, 6);
            hay_par = true;
            esp_bt_hf_volume_control(par, ESP_HF_VOLUME_CONTROL_TARGET_SPK, VOLUMEN);
            esp_bt_hf_volume_control(par, ESP_HF_VOLUME_CONTROL_TARGET_MIC, 15);
            /* El audio se abre en cuanto hay conexión: un transceptor tiene que
               estar escuchando siempre, no sólo durante una «llamada». */
            esp_bt_hf_connect_audio(par);
            abrir_a2dp_en = millis() + 1500;    // y A2DP, para que el micro abra AVRCP
        } else if (p->conn_stat.state == ESP_HF_CONNECTION_STATE_DISCONNECTED) {
            /* Se fue el micro con el PTT abierto: se cierra, o seguiría
               transmitiendo silencio hasta el TOT. */
            if (ptt_origen() == PTT_O_BT) ptt_suelta("se fue el micro");
            bldn_abajo = false;            // al volver, el botón estará arriba
            avrc_abajo = false;
            audio_abierto = false;
        }
        break;
    case ESP_HF_AUDIO_STATE_EVT:
        audio_dice("micro: audio %s", p->audio_stat.state <= 3 ? EST_AUD[p->audio_stat.state] : "?");
        audio_abierto = p->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED ||
                        p->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED_MSBC;
        audio_despierta();
        if (audio_abierto)
            esp_bt_hf_register_data_callback(llega_del_micro, pide_el_altavoz);
        /* Un transceptor escucha SIEMPRE: si el auricular cierra el audio (el
           JBL lo hace al pulsar «tel»), se vuelve a abrir. */
        else if (p->audio_stat.state == ESP_HF_AUDIO_STATE_DISCONNECTED && slc)
            reabrir_en = millis() + 700;
        break;
    case ESP_HF_BVRA_RESPONSE_EVT:
        /* «Asistente de voz»: una pulsación abre, la siguiente cierra. Se le
           confirma, para que el auricular crea que el asistente está activo y
           la siguiente pulsación mande el OFF. */
        audio_dice("micro: boton asistente de voz -> %s", p->vra_rep.value ? "ON" : "OFF");
        ptt_pulsa(PTT_O_BT, p->vra_rep.value);
        esp_bt_hf_vra(par, p->vra_rep.value ? ESP_HF_VR_STATE_ENABLED : ESP_HF_VR_STATE_DISABLED);
        if (!audio_abierto) reabrir_en = millis() + 300;
        break;
    case ESP_HF_ATA_RESPONSE_EVT:
        audio_dice("micro: boton contestar (ATA)");
        esp_bt_hf_cmee_response(p->ata_rep.remote_addr, ESP_HF_AT_RESPONSE_CODE_OK, ESP_HF_CME_AG_FAILURE);
        break;
    case ESP_HF_CHUP_RESPONSE_EVT:
        audio_dice("micro: boton colgar (AT+CHUP)");
        esp_bt_hf_cmee_response(p->chup_rep.remote_addr, ESP_HF_AT_RESPONSE_CODE_OK, ESP_HF_CME_AG_FAILURE);
        break;
    case ESP_HF_DIAL_EVT:
        esp_bt_hf_cmee_response(p->out_call.remote_addr, ESP_HF_AT_RESPONSE_CODE_OK, ESP_HF_CME_AG_FAILURE);
        // ATD<num> trae número; AT+BLDN llega sin él (num_or_loc = NULL, y
        // `type` sin rellenar: no fiarse de él).
        if (p->out_call.num_or_loc)
            audio_dice("micro: boton marcar (%s)", p->out_call.num_or_loc);
        else
            boton_rellamada();
        break;
    case ESP_HF_VOLUME_CONTROL_EVT:
        if (p->volume_control.type == ESP_HF_VOLUME_TYPE_SPK)
            boton_volumen(p->volume_control.volume);
        break;
    case ESP_HF_UNAT_RESPONSE_EVT:
        boton_desconocido(p->unat_rep.unat ? p->unat_rep.unat : "");
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
        audio_dice("micro: DTMF %s", p->vts_rep.code);
        break;
    case ESP_HF_BCS_RESPONSE_EVT:
        audio_dice("micro: codec negociado %d", p->bcs_rep.mode);
        break;
    default:
        break;
    }
}

// -------------------------------------------------------------------- GAP --
static void gap(esp_bt_gap_cb_event_t ev, esp_bt_gap_cb_param_t *p)
{
    switch (ev) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        char nombre[40] = "";
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
        /* Clase «audio/vídeo» (0x04) = auriculares, altavoces, manos libres. */
        bool audio = ((cod >> 8) & 0x1F) == 0x04;
        char m[18];
        mac_txt(p->disc_res.bda, m);
        audio_dice("  %s %4d dBm  %s%s", m, rssi, nombre[0] ? nombre : "(sin nombre)", audio ? "  <- audio" : "");
        if (audio) apunta_candidato(p->disc_res.bda, rssi, nombre);
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (p->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            buscando = true;
            audio_dice("buscando micros Bluetooth (10 s)...");
        } else if (buscando) {
            buscando = false;
            fin_busqueda = true;
            audio_dice("fin de la busqueda");
        }
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        audio_dice("emparejado con %s: %s", p->auth_cmpl.device_name,
                   p->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS ? "OK" : "FALLO");
        if (p->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            /* El nombre es para la PANTALLA: con dos micros en la mesa, una MAC
               no dice cuál está enganchado. Se recuerda como la MAC. */
            snprintf(nombre_par, sizeof nombre_par, "%.23s", (const char *)p->auth_cmpl.device_name);
            guardar_nombre = true;
            audio_despierta();
        }
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
        esp_bt_gap_pin_reply(p->pin_req.bda, true, 4, pin);
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
        esp_bt_gap_ssp_confirm_reply(p->cfm_req.bda, true);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- arranque --
/* EL DOBLE RESET. La LoRa32 V2.1 no tiene más botón que el RST, así que el
   gesto para emparejar es pulsarlo DOS veces seguidas: al arrancar se deja una
   marca en la NVS que se borra a los 3 s; si al arrancar la marca sigue ahí, el
   arranque anterior duró menos de eso. Sólo cuentan los arranques por RST o
   por encendido: un reinicio por fallo (wdt, panic) no es un gesto de nadie. */
static uint32_t t_marca = 0;
static void mira_doble_reset()
{
    esp_reset_reason_t r = esp_reset_reason();
    bool gesto = r == ESP_RST_POWERON || r == ESP_RST_EXT;
    bool marca = prefs.getBool("rst2", false);
    if (gesto && marca) {
        prefs.putBool("rst2", false);
        emp_al_arrancar = true;
    } else if (gesto) {
        prefs.putBool("rst2", true);
        t_marca = millis() | 1;
    } else if (marca) {
        prefs.putBool("rst2", false);
    }
}

bool hfp_arranca()
{
    prefs.begin("hfp", false);
    mira_doble_reset();
    hay_par = prefs.getBytes("par", par, 6) == 6;
    prefs.getString("nom", nombre_par, sizeof nombre_par);
    ptt_propio = prefs.getBool("pttp", false);

    heap_antes = ESP.getFreeHeap();
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t e;
    const char *paso = nullptr;
    if ((e = esp_bt_controller_init(&cfg)) != ESP_OK) paso = "controlador init";
    else if ((e = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) paso = "controlador enable";
    else if ((e = esp_bluedroid_init()) != ESP_OK) paso = "bluedroid init";
    else if ((e = esp_bluedroid_enable()) != ESP_OK) paso = "bluedroid enable";
    if (paso) {
        audio_dice("micro: el Bluetooth NO arranca (%s: %s)", paso, esp_err_to_name(e));
        return false;
    }
    /* UNA ANTENA PARA LOS DOS. Si el nodo tiene WiFi (enlace, clientes), que el
       audio SCO —que no puede esperar— gane los turnos al WiFi, que si puede.
       Medido en el banco: con WiFi se pierde ~10 % del micro y esto lo mejora
       algo; el remedio de verdad es no encender el WiFi. */
    esp_coex_preference_set(ESP_COEX_PREFER_BT);
    esp_bt_dev_set_device_name("PTT LoRa");
    esp_bt_gap_register_callback(gap);
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
    esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin);
    esp_bt_hf_register_callback(manos_libres);
    esp_bt_hf_init(par);
    /* AVRCP antes que A2DP, como en los ejemplos de ESP-IDF. Destino (TG) para
       recibir las teclas del micro; controlador (CT) porque sin el algunos
       auriculares no completan el enlace. */
    esp_avrc_ct_register_callback(avrc_ct);
    esp_avrc_ct_init();
    esp_avrc_tg_register_callback(avrc_tg);
    esp_avrc_tg_init();
    {
        esp_avrc_psth_bit_mask_t teclas = {0};
        esp_avrc_tg_get_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_ALLOWED_CMD, &teclas);
        esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &teclas, ESP_AVRC_PT_CMD_FAST_FORWARD);
        esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &teclas, ESP_AVRC_PT_CMD_REWIND);
        esp_avrc_tg_set_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_SUPPORTED_CMD, &teclas);
    }
    esp_a2d_register_callback(a2dp);
    esp_a2d_source_register_data_callback(a2dp_datos);
    esp_a2d_source_init();
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    rb_mic = xRingbufferCreate(3072, RINGBUF_TYPE_BYTEBUF);
    rb_alt = xRingbufferCreate(3072, RINGBUF_TYPE_BYTEBUF);
    xTaskCreatePinnedToCore(empuja_salida, "salida", 3 * 1024, nullptr, 6, nullptr, 1);
    pila_ok = true;
    heap_pila = heap_antes - ESP.getFreeHeap();

    if (emp_al_arrancar) {
        /* Doble reset: ni se intenta el micro de antes (su llamada ocuparía la
           radio justo cuando hay que buscar). */
        audio_dice("micro: DOBLE RESET -> modo emparejar (pon el micro a emparejar)");
        hfp_buscar(true);
    } else if (hay_par) {
        char m[18];
        mac_txt(par, m);
        audio_dice("micro: Bluetooth Classic listo (%u B), buscando a %s %s", (unsigned)heap_pila, m, nombre_par);
        esp_bt_hf_connect(par);
    } else {
        audio_dice("micro: Bluetooth Classic listo (%u B). Sin micro vinculado: "
                   "ponlo a emparejar y manda `audio bt buscar`", (unsigned)heap_pila);
    }
    return true;
}

void hfp_atiende()
{
    if (!pila_ok) return;
    /* DIAGNOSTICO del zumbido (3-oct-2026): cada 5 s, con audio abierto, el
       ritmo real de lo que va al altavoz y de lo que llega del micro. CVSD
       necesita 16.000 B/s en cada sentido. */
    static uint32_t t_diag = 0, b_alt0 = 0, b_mic0 = 0, pide0 = 0, listo0 = 0, cero0 = 0, hue0 = 0, vac0 = 0, blq0 = 0;
    if (audio_abierto && millis() - t_diag > 5000) {
        uint32_t dt = millis() - t_diag; t_diag = millis();
        size_t cola = 0;
        if (rb_alt) vRingbufferGetInfo(rb_alt, nullptr, nullptr, nullptr, nullptr, &cola);
        audio_dice("diag: alt %lu B/s (pide %lu/s, ceros %lu, huecos %lu, listo %lu/s, cola %u B) | mic %lu B/s (vacios %lu/%lu) | heap %u",
                   (unsigned long)((bytes_alt - b_alt0) * 1000UL / dt), (unsigned long)((n_pide - pide0) * 1000UL / dt),
                   (unsigned long)(n_pide_cero - cero0), (unsigned long)(huecos_alt - hue0),
                   (unsigned long)((n_listo - listo0) * 1000UL / dt), (unsigned)cola,
                   (unsigned long)((bytes_mic - b_mic0) * 1000UL / dt), (unsigned long)(mic_vacios - vac0),
                   (unsigned long)(mic_bloques - blq0), (unsigned)ESP.getFreeHeap());
        b_alt0 = bytes_alt; b_mic0 = bytes_mic; pide0 = n_pide; listo0 = n_listo; cero0 = n_pide_cero;
        hue0 = huecos_alt; vac0 = mic_vacios; blq0 = mic_bloques;
    }
    if (t_marca && millis() - t_marca > 3000) { t_marca = 0; prefs.putBool("rst2", false); }
    if (fin_busqueda) { fin_busqueda = false; acaba_busqueda(); }
    if (emp_fase == 2 && (slc || millis() - emp_t > 20000)) {
        if (!slc) { emp_fase = 3; emp_t = millis(); audio_dice("micro: no ha contestado"); }
        else emp_fase = 0;
        audio_despierta();
    }
    if (emp_fase == 3 && millis() - emp_t > 5000) { emp_fase = 0; audio_despierta(); }
    if (emp_fase == 1) {                      // la cuenta atrás de la pantalla
        static uint32_t t_seg = 0;
        if (millis() - t_seg > 1000) { t_seg = millis(); audio_despierta(); }
    }
    if (guardar_par) { guardar_par = false; prefs.putBytes("par", par, 6); }
    if (guardar_nombre) { guardar_nombre = false; prefs.putString("nom", nombre_par); }
    if (guardar_propio) { guardar_propio = false; prefs.putBool("pttp", ptt_propio); }
    /* Si el micro no está (apagado, fuera de alcance), se insiste cada 15 s:
       encenderlo tiene que bastar para que el transceptor lo coja. */
    static uint32_t t_reintento = 0;
    if (!slc && hay_par && !buscando && !emp_fase && millis() - t_reintento > 15000) {
        t_reintento = millis();
        esp_bt_hf_connect(par);
    }
    if (abrir_a2dp_en && (int32_t)(millis() - abrir_a2dp_en) >= 0) {
        abrir_a2dp_en = 0;
        if (slc && !a2dp_ok && hay_par) { a2dp_mio = true; esp_a2d_source_connect(par); }
    }
    if (rehacer_a2dp) {                       // lo abrio el micro: cerrar y reabrir
        rehacer_a2dp = false;
        audio_dice("micro: A2DP abierto por el micro, se rehace desde la placa");
        esp_a2d_source_disconnect(par);
        abrir_a2dp_en = millis() + 1500;
    }
    /* A2DP SIEMPRE ENLAZADO mientras el micro este: sin el no hay AVRCP (ni
       PTT), y ademas el Abbree mete un ZUMBIDO en su altavoz cuando A2DP esta
       cerrado (oido el 3-oct: con A2DP enlazado desaparecia y volvia en cuanto
       el micro lo cerraba). Si lo cierra, se reabre a los 5 s. */
    static uint32_t t_a2dp = 0;
    if (slc && !a2dp_ok && hay_par && !abrir_a2dp_en && millis() - t_a2dp > 5000) {
        t_a2dp = millis();
        a2dp_mio = true;
        esp_a2d_source_connect(par);
    }
    if (reabrir_en && (int32_t)(millis() - reabrir_en) >= 0) {
        reabrir_en = 0;
        if (slc && !audio_abierto && hay_par) esp_bt_hf_connect_audio(par);
    }
}

// ----------------------------------------------------------------- órdenes --
static void empieza_busqueda()
{
    portENTER_CRITICAL(&cerrojo_cand);
    n_cand = 0;
    portEXIT_CRITICAL(&cerrojo_cand);
    emp_t = millis();
    if (esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 8, 0) != ESP_OK) {
        audio_dice("micro: no se pudo empezar a buscar");
        emp_fase = 0;
    }
}

void hfp_buscar(bool autoconecta)
{
    if (!pila_ok) { audio_dice("micro: no hay Bluetooth"); return; }
    if (buscando) { audio_dice("micro: ya estoy buscando"); return; }
    emp_auto = autoconecta;
    emp_intentos = 0;
    emp_fase = 1;
    audio_despierta();
    empieza_busqueda();
}

/* Al acabar cada vuelta de 10 s. La lista sale SIEMPRE en líneas fijas —la app
   las lee para que el usuario elija—, de más a menos señal. */
static void acaba_busqueda()
{
    Candidato c[8];
    portENTER_CRITICAL(&cerrojo_cand);
    uint8_t n = n_cand;
    memcpy(c, cand, sizeof c);
    portEXIT_CRITICAL(&cerrojo_cand);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && c[j].rssi > c[j - 1].rssi; j--) { Candidato t = c[j]; c[j] = c[j - 1]; c[j - 1] = t; }
    for (int i = 0; i < n; i++) {
        char m[18];
        mac_txt(c[i].mac, m);
        audio_dice("micros: %s|%d|%s", m, c[i].rssi, c[i].nombre);
    }
    audio_dice("micros: fin %u", (unsigned)n);
    if (!emp_auto) { emp_fase = 0; audio_despierta(); return; }
    if (n == 0) {
        if (++emp_intentos < 3) { empieza_busqueda(); return; }
        emp_fase = 3;
        emp_t = millis();
        audio_dice("micro: no aparece ninguno en modo emparejar");
        audio_despierta();
        return;
    }
    hfp_conecta(c[0].mac);
    snprintf(nombre_par, sizeof nombre_par, "%s", c[0].nombre);
    guardar_nombre = true;
    emp_fase = 2;
    emp_t = millis();
    audio_despierta();
}

bool hfp_emparejando(char *l1, char *l2, size_t cap)
{
    switch (emp_fase) {
    case 1: {
        int queda = 10 * (emp_auto ? 3 - emp_intentos : 1) - (int)((millis() - emp_t) / 1000);
        snprintf(l1, cap, "Buscando micro %ds", queda < 0 ? 0 : queda);
        snprintf(l2, cap, "%s", emp_auto ? "ponlo a emparejar" : "(desde la app)");
        return true;
    }
    case 2:
        snprintf(l1, cap, "Conectando con");
        snprintf(l2, cap, "%s", nombre_par[0] ? nombre_par : "el micro");
        return true;
    case 3:
        snprintf(l1, cap, "No encontrado");
        snprintf(l2, cap, "doble RST: repetir");
        return true;
    default:
        return false;
    }
}

bool hfp_conecta(const uint8_t mac[6])
{
    if (!pila_ok) return false;
    esp_bt_gap_cancel_discovery();
    if (slc && memcmp(par, mac, 6)) esp_bt_hf_disconnect(par);
    memcpy(par, mac, 6);
    hay_par = true;
    nombre_par[0] = 0;
    guardar_par = true;
    guardar_nombre = true;
    ptt_propio = false;            // micro nuevo: se vuelve a aprender
    bldn_abajo = false;
    guardar_propio = true;
    char m[18];
    mac_txt(par, m);
    audio_dice("micro: conectando con %s", m);
    return esp_bt_hf_connect(par) == ESP_OK;
}

void hfp_olvida()
{
    if (pila_ok && slc) esp_bt_hf_disconnect(par);
    if (pila_ok && hay_par) esp_bt_gap_remove_bond_device(par);
    hay_par = false;
    nombre_par[0] = 0;
    prefs.remove("par");
    prefs.remove("nom");
    prefs.remove("pttp");
    ptt_propio = false;
    bldn_abajo = false;
    audio_dice("micro: olvidado");
    audio_despierta();
}

void hfp_estado(char *s, size_t cap)
{
    char m[18] = "ninguno";
    if (hay_par) mac_txt(par, m);
    snprintf(s, cap,
             "micro: %s %s slc=%s audio=%s a2dp=%s avrcp=%s ptt=%s pila=%uB rx=%luB vacios=%lu/%lu tirados=%lu "
             "tx=%luB huecos=%lu agc=x%.1f rms=%lu pico=%lu",
             m, nombre_par[0] ? nombre_par : "-", slc ? "si" : "no", audio_abierto ? "si" : "no",
             a2dp_ok ? "si" : "no", avrc_ok ? "si" : "no",
             ptt_propio ? (bldn_abajo ? "propio(abajo)" : "propio") : "volumen",
             (unsigned)heap_pila, (unsigned long)bytes_mic, (unsigned long)mic_vacios,
             (unsigned long)mic_bloques, (unsigned long)mic_tirados, (unsigned long)bytes_alt,
             (unsigned long)huecos_alt, g_mic, (unsigned long)rms_mic, (unsigned long)pico_mic);
    pico_mic = 0;
}

#endif  // AUDIO_LOCAL && AUDIO_BT
