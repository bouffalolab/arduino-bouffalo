// WiFi checks cover connection/addresses, repeated begin, 20 gateway pings, DNS, gateway TCP port 80 and an invalid passphrase.
// Also check missing SSID, disconnect and reconnect. Print "<item> <values>..." per result, then "DONE".
// Tracked arduino_secrets.h has empty credentials for compilation only.
#include <WiFi.h>

#include "arduino_secrets.h"

/**
 * @brief Print a tagged test result and its numeric fields.
 */
static void line(const char *item, long a, long b = 0, long c = 0)
{
    Serial.print(item);
    Serial.print(' ');
    Serial.print(a);
    Serial.print(' ');
    Serial.print(b);
    Serial.print(' ');
    Serial.println(c);
}

/**
 * @brief Print a tagged test IPv4 address.
 */
static void addr(const char *item, IPAddress ip)
{
    Serial.print(item);
    Serial.print(' ');
    Serial.println(ip);
}

/**
 * @brief A timed-out begin can still connect in the background; wait for its result.
 */
static int settle(int st)
{
    unsigned long start = millis();
    while (st == WL_IDLE_STATUS && millis() - start < 20000) {
        delay(100);
        st = WiFi.status();
    }
    return st;
}

/**
 * @brief Call begin and print "<item> <status> <reason> <elapsed-ms>".
 */
static int connect(const char *item, const char *ssid, const char *pass)
{
    unsigned long start = millis();
    int st = WiFi.begin(ssid, pass);
    unsigned long took = millis() - start;
    st = settle(st);
    line(item, st, WiFi.reasonCode(), took);
    return st;
}

/**
 * @brief Exercise TCP communication with the configured gateway test endpoint.
 */
static void tcpGateway()
{
    WiFiClient client;
    if (!client.connect(WiFi.gatewayIP(), 80)) {
        line("TCP", 0);
        return;
    }
    client.print("GET / HTTP/1.0\r\n\r\n");
    String first;
    long total = 0;
    unsigned long start = millis();
    while ((client.connected() || client.available()) && millis() - start < 5000) {
        int c = client.read();
        if (c < 0) {
            delay(1);
            continue;
        }
        total++;
        if (first.length() < 40 && c != '\r' && c != '\n' && total == (long)first.length() + 1) {
            first += (char)c;
        }
    }
    client.stop();
    Serial.print("TCP 1 ");
    Serial.print(total);
    Serial.print(' ');
    Serial.println(first);
}

/**
 * @brief Initialize the resources used by this example or validation sketch.
 */
void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 10000) {
    }
    uint8_t mac[6];
    WiFi.macAddress(mac);
    Serial.print("MAC ");
    for (int i = 0; i < 6; i++) {
        if (mac[i] < 16) {
            Serial.print('0');
        }
        Serial.print(mac[i], HEX);
    }
    Serial.println();
    Serial.print("FW ");
    Serial.println(WiFi.firmwareVersion());
    line("STATUS0", WiFi.status());

    if (connect("BEGIN", SECRET_SSID, SECRET_PASS) == WL_CONNECTED) {
        addr("IP", WiFi.localIP());
        addr("MASK", WiFi.subnetMask());
        addr("GW", WiFi.gatewayIP());
        addr("DNS0", WiFi.dnsIP(0));
        addr("DNS1", WiFi.dnsIP(1));
        line("RSSI", WiFi.RSSI());
        Serial.print("SSID ");
        Serial.println(WiFi.SSID());
        unsigned long start = millis();
        line("REBEGIN", WiFi.begin(SECRET_SSID, SECRET_PASS), 0, millis() - start);

        int ok = 0;
        for (int i = 0; i < 20; i++) {
            int rtt = WiFi.ping(WiFi.gatewayIP());
            line("PING", i, rtt);
            ok += rtt >= 0;
            delay(200);
        }
        line("PINGOK", ok, 20);
        IPAddress host;
        int found = WiFi.hostByName("www.arduino.cc", host);
        Serial.print("DNS ");
        Serial.print(found);
        Serial.print(' ');
        Serial.println(host);
        tcpGateway();
    }

    connect("BADPASS", SECRET_SSID, "wrong-password-0");
    connect("NOSSID", "bl616cl-no-such-ssid", "12345678");
    line("DISC", WiFi.disconnect(), WiFi.status());
    if (connect("BEGIN2", SECRET_SSID, SECRET_PASS) == WL_CONNECTED) {
        addr("IP2", WiFi.localIP());
        line("PING2", WiFi.ping(WiFi.gatewayIP(), 128, 3));
    }

    // Disconnect during association must remain disconnected even if association finishes after its 3 s wait; a later connection must still work.
    // Print "DISCMID <status-after-15s> <ever-connected> <disconnect-ms>".
    WiFi.disconnect();
    WiFi.setTimeout(0);
    WiFi.begin(SECRET_SSID, SECRET_PASS);
    unsigned long start = millis();
    WiFi.disconnect();
    unsigned long took = millis() - start;
    int seen = 0;
    while (millis() - start < 15000) {
        seen |= WiFi.status() == WL_CONNECTED;
        delay(100);
    }
    line("DISCMID", WiFi.status(), seen, took);
    WiFi.setTimeout(10000);
    if (connect("BEGIN3", SECRET_SSID, SECRET_PASS) == WL_CONNECTED) {
        line("PING3", WiFi.ping(WiFi.gatewayIP(), 128, 3));
    }
    Serial.println("DONE");
}

/**
 * @brief Service the example or validation scenario on each Arduino loop iteration.
 */
void loop()
{
    delay(5000);
    line("ALIVE", WiFi.status(), WiFi.RSSI(), millis());
}
