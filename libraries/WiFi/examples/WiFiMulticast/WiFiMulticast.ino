#include <WiFi.h>
#include <WiFiUdp.h>
#include "arduino_secrets.h"

WiFiUDP udp;
const IPAddress group(239, 1, 2, 3);
const uint16_t port = 4210;

void setup()
{
    Serial.begin(115200);
    WiFi.begin(SECRET_SSID, SECRET_PASS);
    while (WiFi.status() != WL_CONNECTED) {
        delay(100);
    }
    if (!udp.beginMulticast(group, port)) {
        Serial.println("Multicast join failed");
    }
}

void loop()
{
    if (udp.parsePacket()) {
        Serial.print("From ");
        Serial.print(udp.remoteIP());
        Serial.print(':');
        Serial.println(udp.remotePort());
        while (udp.available()) {
            Serial.write(udp.read());
        }
        Serial.println();
    }
    static unsigned long last;
    if (millis() - last >= 5000) {
        last = millis();
        if (udp.beginMulticastPacket()) {
            udp.print("Hello multicast");
            if (!udp.endPacket()) {
                Serial.println("Multicast send failed");
            }
        }
    }
    delay(1);
}
