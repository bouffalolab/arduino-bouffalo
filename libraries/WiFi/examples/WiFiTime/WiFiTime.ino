#include <WiFi.h>
#include "arduino_secrets.h"

void setup()
{
    Serial.begin(115200);
    WiFi.setHostname("arduino-bl");
    WiFi.begin(SECRET_SSID, SECRET_PASS);
}

void loop()
{
    unsigned long now = WiFi.getTime();
    if (now == 0) {
        Serial.println("Waiting for SNTP");
    } else {
        Serial.print("UTC Unix seconds: ");
        Serial.println(now);
    }
    delay(1000);
}
