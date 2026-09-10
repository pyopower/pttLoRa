/* ¿HAY ALGUIEN EN EL BUS?
 *
 * Cuando el chip de radio no contesta, hay tres explicaciones y desde el SPI no
 * se distinguen: que no este alimentado, que no este soldado, o que una pista
 * este cortada o a masa. Esto lo separa mirando los pines como lo que son,
 * patas electricas, en vez de como un bus.
 *
 * El metodo: poner cada pin como entrada con pull-up interno y leerlo, y luego
 * con pull-down. Un pin AL AIRE sigue al resistor —sube con pull-up y baja con
 * pull-down—. Un pin sujeto por algo externo se queda donde ese algo lo tenga,
 * gane quien gane al resistor interno (que son unos 45 kΩ, muy flojos).
 *
 *   sube y baja        -> al aire: no hay nada conectado ahi
 *   siempre 0          -> a masa, o alguien tirando fuerte hacia abajo
 *   siempre 1          -> a 3V3, o con pull-up externo (normal en un CS)
 */
#include <Arduino.h>

struct P { const char *que; int pin; };
static const P PINES[] = {
    {"SCK  (esperado 5) ",  5},
    {"MISO (esperado 19)", 19},
    {"MOSI (esperado 27)", 27},
    {"CS   (esperado 18)", 18},
    {"RST  (esperado 23)", 23},
    {"DIO0 (esperado 26)", 26},
    {"DIO1 (esperado 33)", 33},
};

void setup()
{
    Serial.begin(115200);
    delay(2500);
    Serial.println("\n=== estado electrico del bus de radio ===\n");
    for (auto &p : PINES) {
        pinMode(p.pin, INPUT_PULLUP);   delay(30);
        int arriba = digitalRead(p.pin);
        pinMode(p.pin, INPUT_PULLDOWN); delay(30);
        int abajo  = digitalRead(p.pin);
        pinMode(p.pin, INPUT);
        const char *veredicto =
            (arriba == 1 && abajo == 0) ? "AL AIRE (nada conectado)" :
            (arriba == 0 && abajo == 0) ? "sujeto a MASA" :
            (arriba == 1 && abajo == 1) ? "sujeto a 3V3 / pull-up externo" :
                                          "raro (mira el cableado)";
        Serial.printf("GPIO%-3d %s  pullup=%d pulldown=%d  -> %s\n",
                      p.pin, p.que, arriba, abajo, veredicto);
    }
    /* ⚠️ MEDIDO en una LoRa32 v2.1 que FUNCIONA (nodoCASA, 10-sep-2026), y no
       es lo que uno supondria — aqui habia escrito que SCK/MOSI/MISO "no deben
       salir al aire" y es FALSO:

         placa BUENA:  SCK al aire · MISO a 3V3 · MOSI al aire · CS al aire
                       RST al aire · DIO0 masa  · DIO1 masa
         placa MUERTA: TODO a masa menos RST

       Lo que delata a un chip sin alimentacion no es que el bus este suelto,
       sino lo contrario: que TODAS las lineas esten clavadas a masa. Un CMOS
       sin Vcc se come el bus por sus diodos de proteccion. */
    Serial.println("\nReferencia medida en una LoRa32 v2.1 que FUNCIONA:");
    Serial.println("  SCK al aire | MISO a 3V3 | MOSI al aire | CS al aire");
    Serial.println("  RST al aire | DIO0 masa  | DIO1 masa");
    Serial.println("Si en cambio sale TODO a masa, el chip de radio no tiene");
    Serial.println("alimentacion o esta en corto: es averia, no configuracion.");
}
void loop() { delay(1000); }
