// Sketch minimo de diagnostico. Manda 'T' por serie para transmitir un paquete;
// el resto del tiempo esta a la escucha e imprime todo lo que ve, incluido el
// nivel del pin DIO0, para saber si la radio avisa de recepcion o no.
#include <Arduino.h>
#include <RadioLib.h>
#ifdef PLACA_TBEAM
  #define XPOWERS_CHIP_AXP2101
  #include <XPowersLib.h>
  XPowersPMU pmu;
#endif

#define P_CS 18
#define P_DIO0 26
#define P_RST 23
#define P_DIO1 33
SX1278 radio = new Module(P_CS, P_DIO0, P_RST, P_DIO1);

volatile bool bandera = false;
volatile uint32_t n_irq = 0;
ICACHE_RAM_ATTR void setFlag() { bandera = true; n_irq++; }

uint32_t t = 0, n_tx = 0, n_rx = 0;

void setup()
{
    Serial.begin(115200);
    delay(2000);
    Serial.println("\n=== minimo ===");
#ifdef PLACA_TBEAM
    Wire.begin(21, 22);
    bool ok = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, 21, 22);
    Serial.printf("pmu=%d\n", (int)ok);
    if (ok) { pmu.setALDO2Voltage(3300); pmu.enableALDO2();
              pmu.setALDO3Voltage(3300); pmu.enableALDO3(); delay(100); }
#endif
    SPI.begin(5, 19, 27, P_CS);
    int st = radio.begin(FRQ, BWD, SFC, CRC4, SYW, PWR, 8);
    Serial.printf("begin=%d\n", st);
    radio.setPacketReceivedAction(setFlag);
    st = radio.startReceive();
    Serial.printf("startReceive=%d\n", st);
}

void loop()
{
    if (bandera) {
        bandera = false;
        uint8_t b[64];
        int len = radio.getPacketLength();
        int st = radio.readData(b, len);
        Serial.printf("RX st=%d len=%d rssi=%.0f snr=%.1f  ->%.*s\n",
                      st, len, radio.getRSSI(), radio.getSNR(), len, (char *)b);
        n_rx++;
        radio.startReceive();
    }
    if (Serial.available()) {
        int c = Serial.read();
        if (c == 'T') {
            char msg[24];
            snprintf(msg, sizeof msg, "PRUEBA-%lu", ++n_tx);
            int st = radio.transmit((uint8_t *)msg, strlen(msg));
            Serial.printf("TX st=%d '%s'\n", st, msg);
            radio.startReceive();
        }
    }
    if (millis() - t > 2000) {
        t = millis();
        Serial.printf("... dio0=%d irq=%lu rx=%lu rssi=%.0f\n",
                      digitalRead(P_DIO0), n_irq, n_rx, radio.getRSSI(false, true));
    }
}
