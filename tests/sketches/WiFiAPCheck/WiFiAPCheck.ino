// AP check driven by host USB commands after "READY".
// AP <ssid> <pass|-> [channel]: select the corresponding beginAP overload; '-' omits the passphrase. Reply "AP <result> <status> <ms>".
// CFG <address> [DNS [gateway [mask]]]: select config by argument count; reply "CFG".
// ADDR: reply "ADDR <status> <address> <mask> <gateway> <softAPIP> <MAC>".
// SSID: reply "SSID <SSID()>|<softAPSSID()>".
// SRV <port>: start TCP; echo each received line and close, printing "SRVLINE <line>".
// Reply "SRV <1/0>".
// UDP <port>: echo incoming datagrams, print "UDPLINE <length>" and reply "UDP <1/0>".
// PING <address>: call WiFi.ping(IPAddress); reply "PING <result>".
// BEGIN: use the arduino_secrets.h network; reply "BEGIN <status> <ms>".
// DISC: call WiFi.disconnect; reply "DISC <status>".
// END: call WiFi.end; reply "END <status>".
// Network credentials are empty in tracked arduino_secrets.h; this copy supports compilation only.
#include <WiFi.h>

#include "arduino_secrets.h"

static WiFiServer server(80);
static bool serving;
static WiFiUDP udp;

/**
 * @brief Parse a test-command IPv4 argument into an IPAddress.
 */
static IPAddress ip_arg(const char *s)
{
    IPAddress a;
    a.fromString(s);
    return a;
}

/**
 * @brief Start the test AP with command-supplied association options.
 */
static void cmd_ap(char **args, int n)
{
    const char *pass = strcmp(args[1], "-") == 0 ? nullptr : args[1];
    unsigned long start = millis();
    uint8_t r;
    if (n == 2) {
        r = pass != nullptr ? WiFi.beginAP(args[0], pass) : WiFi.beginAP(args[0]);
    } else {
        uint8_t ch = (uint8_t)atoi(args[2]);
        r = pass != nullptr ? WiFi.beginAP(args[0], pass, ch) : WiFi.beginAP(args[0], ch);
    }
    Serial.print("AP ");
    Serial.print(r);
    Serial.print(' ');
    Serial.print(WiFi.status());
    Serial.print(' ');
    Serial.println(millis() - start);
}

/**
 * @brief Apply test-command IPv4 configuration and report the result.
 */
static void cmd_cfg(char **args, int n)
{
    if (n == 1) {
        WiFi.config(ip_arg(args[0]));
    } else if (n == 2) {
        WiFi.config(ip_arg(args[0]), ip_arg(args[1]));
    } else if (n == 3) {
        WiFi.config(ip_arg(args[0]), ip_arg(args[1]), ip_arg(args[2]));
    } else {
        WiFi.config(ip_arg(args[0]), ip_arg(args[1]), ip_arg(args[2]), ip_arg(args[3]));
    }
    Serial.println("CFG");
}

/**
 * @brief Print the network addresses used by the test sketch.
 */
static void cmd_addr()
{
    uint8_t mac[6];
    char m[18];
    WiFi.macAddress(mac);
    snprintf(m, sizeof(m), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.print("ADDR ");
    Serial.print(WiFi.status());
    Serial.print(' ');
    Serial.print(WiFi.localIP());
    Serial.print(' ');
    Serial.print(WiFi.subnetMask());
    Serial.print(' ');
    Serial.print(WiFi.gatewayIP());
    Serial.print(' ');
    Serial.print(WiFi.softAPIP());
    Serial.print(' ');
    Serial.println(m);
}

/**
 * @brief Read one client line with a 5 s bound, echo it, then close.
 */
static void serve()
{
    WiFiClient c = server.available();
    if (!c) {
        return;
    }
    String text;
    unsigned long start = millis();
    while (c.connected() && millis() - start < 5000) {
        int b = c.read();
        if (b < 0) {
            delay(1);
        } else if (b == '\n') {
            break;
        } else if (b != '\r') {
            text += (char)b;
        }
    }
    c.println(text);
    c.stop();
    Serial.print("SRVLINE ");
    Serial.println(text);
}

/**
 * @brief Echo received UDP data through the test socket.
 */
static void echo_udp()
{
    int n = udp.parsePacket();
    if (n <= 0) {
        return;
    }
    uint8_t buf[512];
    int got = udp.read(buf, sizeof(buf));
    udp.beginPacket(udp.remoteIP(), udp.remotePort());
    udp.write(buf, got > 0 ? got : 0);
    udp.endPacket();
    Serial.print("UDPLINE ");
    Serial.println(got);
}

/**
 * @brief Initialize the resources used by this example or validation sketch.
 */
void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 10000) {
    }
    Serial.println("READY");
}

/**
 * @brief Service the example or validation scenario on each Arduino loop iteration.
 */
void loop()
{
    static String line;
    if (serving) {
        serve();
    }
    echo_udp();
    while (Serial.available() > 0) {
        int b = Serial.read();
        if (b != '\n') {
            if (b != '\r') {
                line += (char)b;
            }
            continue;
        }
        char buf[200];
        line.toCharArray(buf, sizeof(buf));
        line = "";
        char *args[6];
        int n = 0;
        char *cmd = strtok(buf, " ");
        while (n < 6 && (args[n] = strtok(nullptr, " ")) != nullptr) {
            n++;
        }
        if (cmd == nullptr) {
            continue;
        }
        if (strcmp(cmd, "AP") == 0 && (n == 2 || n == 3)) {
            cmd_ap(args, n);
        } else if (strcmp(cmd, "CFG") == 0 && n >= 1 && n <= 4) {
            cmd_cfg(args, n);
        } else if (strcmp(cmd, "ADDR") == 0) {
            cmd_addr();
        } else if (strcmp(cmd, "SSID") == 0) {
            Serial.print("SSID ");
            Serial.print(WiFi.SSID());
            Serial.print('|');
            Serial.println(WiFi.softAPSSID());
        } else if (strcmp(cmd, "SRV") == 0 && n == 1) {
            server.begin(atoi(args[0]));
            serving = (bool)server;
            Serial.print("SRV ");
            Serial.println(serving ? 1 : 0);
        } else if (strcmp(cmd, "UDP") == 0 && n == 1) {
            Serial.print("UDP ");
            Serial.println(udp.begin(atoi(args[0])));
        } else if (strcmp(cmd, "PING") == 0 && n == 1) {
            Serial.print("PING ");
            Serial.println(WiFi.ping(ip_arg(args[0])));
        } else if (strcmp(cmd, "BEGIN") == 0) {
            unsigned long start = millis();
            int st = WiFi.begin(SECRET_SSID, SECRET_PASS);
            Serial.print("BEGIN ");
            Serial.print(st);
            Serial.print(' ');
            Serial.println(millis() - start);
        } else if (strcmp(cmd, "DISC") == 0) {
            WiFi.disconnect();
            Serial.print("DISC ");
            Serial.println(WiFi.status());
        } else if (strcmp(cmd, "END") == 0) {
            WiFi.end();
            Serial.print("END ");
            Serial.println(WiFi.status());
        } else {
            Serial.println("ERR");
        }
    }
}
