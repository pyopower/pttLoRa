/* Lo que pesa NimBLE, para saber qué se recupera si un firmware dedicado al
 * micrófono Bluetooth renuncia al enlace con el móvil. */
#include <Arduino.h>
#include <NimBLEDevice.h>
void setup() {
    Serial.begin(115200);
    NimBLEDevice::init("PTTLoRa");
    NimBLEServer *s = NimBLEDevice::createServer();
    NimBLEService *v = s->createService("ffe0");
    v->createCharacteristic("ffe1", NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::WRITE_NR);
    v->start();
    NimBLEDevice::getAdvertising()->start();
}
void loop() { delay(1000); }
