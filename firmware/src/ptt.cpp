/* ptt.cpp — ver ptt.h. */
#ifdef AUDIO_LOCAL

#include "ptt.h"

/* Lo tocan tres tareas: loop() (botones físicos, consola, cortes del nodo), la
   pila Bluetooth (botones del micro) y la de audio (que sólo lee). Las
   transiciones van bajo cerrojo para que un "abajo" del micro y un corte del
   TOT que lleguen a la vez no dejen el estado a medias. */
static portMUX_TYPE cerrojo = portMUX_INITIALIZER_UNLOCKED;
static volatile bool    activo = false;
static volatile uint8_t origen = PTT_O_NADIE;
static const char      *corte = "";

static int     pin_placa = -1, pin_ext = -1;
static uint8_t modo_boton = BOTON_PULSADOR;

static const uint32_t REBOTE_MS = 30;

void ptt_arranca(int placa, int externo, uint8_t modo)
{
    pin_placa = placa;
    pin_ext = externo;
    modo_boton = modo;
    if (pin_placa >= 0) pinMode(pin_placa, INPUT_PULLUP);
    if (pin_ext >= 0) pinMode(pin_ext, INPUT_PULLUP);
}

void ptt_pulsa(uint8_t o, bool abajo)
{
    portENTER_CRITICAL(&cerrojo);
    if (abajo && !activo) {
        activo = true;
        origen = o;
        corte = "";
    } else if (!abajo && activo && origen == o) {
        /* Sólo suelta quien pulsó: soltar el botón de la placa no debe cerrarle
           el micro a quien está hablando con el Bluetooth. */
        activo = false;
        origen = PTT_O_NADIE;
    }
    portEXIT_CRITICAL(&cerrojo);
}

void ptt_conmuta(uint8_t o)
{
    portENTER_CRITICAL(&cerrojo);
    if (!activo) {
        activo = true;
        origen = o;
        corte = "";
    } else {
        /* Cualquiera cierra un conmutador: si el micro Bluetooth se quedó
           abierto, el botón de la placa tiene que poder cerrarlo. */
        activo = false;
        origen = PTT_O_NADIE;
    }
    portEXIT_CRITICAL(&cerrojo);
}

void ptt_suelta(const char *motivo)
{
    portENTER_CRITICAL(&cerrojo);
    if (activo) corte = motivo ? motivo : "";
    activo = false;
    origen = PTT_O_NADIE;
    portEXIT_CRITICAL(&cerrojo);
}

bool ptt_activo() { return activo; }
uint8_t ptt_origen() { return origen; }
const char *ptt_ultimo_corte() { return corte; }
void ptt_olvida_corte() { corte = ""; }

const char *ptt_nombre_origen(uint8_t o)
{
    switch (o) {
    case PTT_O_PLACA:   return "boton";
    case PTT_O_PIN:     return "pulsador";
    case PTT_O_BT:      return "micro BT";
    case PTT_O_CONSOLA: return "consola";
    default:            return "-";
    }
}

void ptt_modo_boton(uint8_t m)
{
    if (m > BOTON_CONMUTADOR) m = BOTON_PULSADOR;
    /* Al cambiar de modo con el botón pulsado, que no se quede abierto. */
    if (origen == PTT_O_PLACA) ptt_pulsa(PTT_O_PLACA, false);
    modo_boton = m;
}

uint8_t ptt_modo_boton() { return modo_boton; }

/* Un botón con su antirrebote. Se trabaja por FLANCOS, no por nivel: así un
   botón que se queda pulsado después de que el nodo cortara (TOT, canal
   ocupado) no vuelve a abrir el micro solo. Hay que soltarlo y volver a
   pulsar, que es lo que se espera de una radio. */
struct Boton {
    int pin = -1;
    bool estable = false;           // true = pulsado
    bool leido = false;
    uint32_t t_cambio = 0;

    /* 1 = flanco de bajada (se pulsa), -1 = de subida (se suelta), 0 = nada. */
    int flanco()
    {
        if (pin < 0) return 0;
        bool ahora = digitalRead(pin) == LOW;
        if (ahora != leido) { leido = ahora; t_cambio = millis(); return 0; }
        if (ahora == estable || millis() - t_cambio < REBOTE_MS) return 0;
        estable = ahora;
        return ahora ? 1 : -1;
    }
};

static Boton b_placa, b_ext;

void ptt_atiende()
{
    b_placa.pin = pin_placa;
    b_ext.pin = pin_ext;

    int f = b_placa.flanco();
    if (f && modo_boton == BOTON_PULSADOR) ptt_pulsa(PTT_O_PLACA, f > 0);
    else if (f > 0 && modo_boton == BOTON_CONMUTADOR) ptt_conmuta(PTT_O_PLACA);

    /* El pulsador externo es siempre de mantener: es lo que hace el PTT de un
       micro de mano con cable. */
    f = b_ext.flanco();
    if (f) ptt_pulsa(PTT_O_PIN, f > 0);
}

#endif  // AUDIO_LOCAL
