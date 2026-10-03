/* ¿Cuánto pesa Bluetooth Classic con HFP? Sólo para medir: se inicializa la
 * pila y se registra el manos libres, que es lo que arrastra el código al
 * binario. No funciona como manos libres de verdad —el controlador del core
 * viene con MAX_SYNC_CONN=0, o sea sin canal de audio— pero para pesar sirve. */
#include <Arduino.h>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_ag_api.h"

static void cb(esp_hf_cb_event_t e, esp_hf_cb_param_t *p) { (void)e; (void)p; }

void setup() {
    Serial.begin(115200);
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_init(&cfg);
    esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    esp_bluedroid_init();
    esp_bluedroid_enable();
    esp_bt_dev_set_device_name("PTTLoRa");
    esp_bt_hf_register_callback(cb);
    esp_bt_hf_init(NULL);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    Serial.println("pila Classic + HFP AG en marcha");
}
void loop() { delay(1000); }
