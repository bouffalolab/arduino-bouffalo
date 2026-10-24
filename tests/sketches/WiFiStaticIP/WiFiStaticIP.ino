// 静态地址检查：经 USB 串口输出 "READY" 后按命令执行，由电脑上的脚本驱动。
//   CFG <地址> [<DNS> [<网关> [<掩码>]]]  按参数个数调用 WiFi.config 的对应重载，输出 "CFG"
//   DNS <DNS1> [<DNS2>]                   WiFi.setDNS，输出 "DNS"
//   BEGIN                                 WiFi.begin，输出 "BEGIN <状态> <毫秒>"
//   DISC                                  WiFi.disconnect，输出 "DISC"
//   ADDR                                  输出 "ADDR <状态> <地址> <掩码> <网关> <DNS0> <DNS1>"
//   PING <主机>                           WiFi.ping，输出 "PING <结果>"
//   RESOLVE <主机>                        WiFi.hostByName，输出 "RESOLVE <1/0> <地址>"
//   TCP <主机> <端口> <文本>              WiFiClient 发一行读回一行，输出 "TCP <连接成功 1/0> <读回的行>"；文本不含空格
// 网络名与密码在 arduino_secrets.h（仓库中为空，只能编译检查）。
#include <WiFi.h>

#include "arduino_secrets.h"

static IPAddress ip_arg(const char *s)
{
    IPAddress a;
    a.fromString(s);
    return a;
}

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

void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 10000) {
    }
    Serial.println("READY");
}

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
