// Wi-Fi and BLE Controller in one image: start the Controller, scan Wi-Fi once, then print
// "BLE <begin-1/0>", "SCAN <count>" and "DONE". The map check requires one copy of the shared RF records.
#include <BLEController.h>
#include <WiFi.h>

/**
 * @brief Initialize the resources used by this example or validation sketch.
 */
void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 10000) {
    }
    Serial.print("BLE ");
    Serial.println(BLEController.begin());
    Serial.print("SCAN ");
    Serial.println(WiFi.scanNetworks());
    Serial.println("DONE");
}

/**
 * @brief Service the example or validation scenario on each Arduino loop iteration.
 */
void loop()
{
    delay(1000);
}
