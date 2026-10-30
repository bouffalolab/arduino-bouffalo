// WiFiServer check: available() serves port 5558 and accept() serves 5559; a host script provides clients.
// Port 5558 uppercases normal lines; "bc" broadcasts "all\\n" and "stop" replies "bye\\n" before stop().
// Port 5559 sends "hi\\n" after accept(), echoes a line then stops. USB reports "IP <address>" and "READY".
// Print "LINE <port> <content>" per handled line. Tracked arduino_secrets.h has empty credentials for compilation only.
#include <WiFi.h>

#include "arduino_secrets.h"

WiFiServer server(5558);
WiFiServer acceptor(5559);

/**
 * @brief Read a line without its newline, waiting at most 1 s.
 */
static bool readLine(WiFiClient &c, String &line)
{
    unsigned long start = millis();
    line = "";
    while (millis() - start < 1000) {
        int b = c.read();
        if (b < 0) {
            delay(1);
        } else if (b == '\n') {
            return true;
        } else {
            line += (char)b;
        }
    }
    return false;
}

/**
 * @brief Initialize the resources used by this example or validation sketch.
 */
void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 10000) {
    }
    int st = WiFi.begin(SECRET_SSID, SECRET_PASS);
    Serial.print("BEGIN ");
    Serial.println(st);
    Serial.print("IP ");
    Serial.println(WiFi.localIP());
    server.begin();
    acceptor.begin();
    Serial.print("SERVERS ");
    Serial.print((bool)server);
    Serial.print(' ');
    Serial.println((bool)acceptor);
    Serial.println("READY");
}

/**
 * @brief Service the example or validation scenario on each Arduino loop iteration.
 */
void loop()
{
    String line;
    WiFiClient c = server.available();
    if (c && readLine(c, line)) {
        Serial.print("LINE 5558 ");
        Serial.println(line);
        if (line == "bc") {
            server.write((const uint8_t *)"all\n", 4);
        } else if (line == "stop") {
            c.print("bye\n");
            c.stop();
        } else {
            line.toUpperCase();
            c.print(line + "\n");
        }
    }
    WiFiClient a = acceptor.accept();
    if (a) {
        a.print("hi\n");
        if (readLine(a, line)) {
            Serial.print("LINE 5559 ");
            Serial.println(line);
        }
        a.stop();
    }
}
