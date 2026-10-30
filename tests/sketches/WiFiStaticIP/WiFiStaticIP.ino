// Static-IP check driven by host USB commands after "READY".
// CFG <address> [DNS [gateway [mask]]]: choose the config overload; reply "CFG".
// DNS <DNS1> [DNS2]: call setDNS; reply "DNS".
// BEGIN: call begin; reply "BEGIN <status> <ms>".
// DISC: call disconnect; reply "DISC".
// ADDR: reply "ADDR <status> <address> <mask> <gateway> <DNS0> <DNS1>".
// PING <host>: reply "PING <result>".
// RESOLVE <host>: call hostByName; reply "RESOLVE <1/0> <address>".
// TCP <host> <port> <text>: send/read one line; reply "TCP <connected-1/0> <line>". Text contains no spaces.
// Tracked arduino_secrets.h has empty credentials for compilation only.
#include <WiFi.h>

#include "arduino_secrets.h"

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
    Serial.print("ADDR ");
    Serial.print(WiFi.status());
    Serial.print(' ');
    Serial.print(WiFi.localIP());
    Serial.print(' ');
    Serial.print(WiFi.subnetMask());
    Serial.print(' ');
    Serial.print(WiFi.gatewayIP());
    Serial.print(' ');
    Serial.print(WiFi.dnsIP(0));
    Serial.print(' ');
    Serial.println(WiFi.dnsIP(1));
}

/**
 * @brief Exchange a supplied test payload with the selected TCP peer.
 */
static void cmd_tcp(const char *host, uint16_t port, const char *text)
{
    WiFiClient c;
    bool ok = c.connect(host, port);
    String back;
    if (ok) {
        c.println(text);
        unsigned long start = millis();
        while (millis() - start < 5000) {
            int b = c.read();
            if (b < 0) {
                delay(1);
            } else if (b == '\n') {
                break;
            } else if (b != '\r') {
                back += (char)b;
            }
        }
    }
    c.stop();
    Serial.print("TCP ");
    Serial.print(ok ? 1 : 0);
    Serial.print(' ');
    Serial.println(back);
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
        if (strcmp(cmd, "CFG") == 0 && n >= 1 && n <= 4) {
            cmd_cfg(args, n);
        } else if (strcmp(cmd, "DNS") == 0 && n >= 1) {
            if (n == 1) {
                WiFi.setDNS(ip_arg(args[0]));
            } else {
                WiFi.setDNS(ip_arg(args[0]), ip_arg(args[1]));
            }
            Serial.println("DNS");
        } else if (strcmp(cmd, "BEGIN") == 0) {
            unsigned long start = millis();
            int st = WiFi.begin(SECRET_SSID, SECRET_PASS);
            Serial.print("BEGIN ");
            Serial.print(st);
            Serial.print(' ');
            Serial.println(millis() - start);
        } else if (strcmp(cmd, "DISC") == 0) {
            WiFi.disconnect();
            Serial.println("DISC");
        } else if (strcmp(cmd, "ADDR") == 0) {
            cmd_addr();
        } else if (strcmp(cmd, "PING") == 0 && n == 1) {
            Serial.print("PING ");
            Serial.println(WiFi.ping(args[0]));
        } else if (strcmp(cmd, "RESOLVE") == 0 && n == 1) {
            IPAddress a;
            int ok = WiFi.hostByName(args[0], a);
            Serial.print("RESOLVE ");
            Serial.print(ok);
            Serial.print(' ');
            Serial.println(a);
        } else if (strcmp(cmd, "TCP") == 0 && n == 3) {
            cmd_tcp(args[0], atoi(args[1]), args[2]);
        } else {
            Serial.println("ERR");
        }
    }
}
