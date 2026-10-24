// WiFiSSLClient 检查：连网后经 USB 串口输出 "IP <地址>"、"READY"，之后按串口命令建立 TLS 连接，由电脑上的脚本驱动。
//   GET <主机> <端口> <路径>   发 HTTP/1.1 GET（Connection: close），读到对端关闭或 10 s，
//                              输出 "RES <连接成功 1/0> <连接毫秒> <收到字节数> <各字节之和> <第一行>"
//   ECHO <主机> <端口> <文本>  发一行，读回一行，输出 "ECHO <连接成功 1/0> <连接毫秒> <读回的行>"
//   MULTI <个数> <主机> <端口> 同时保持多个连接，输出 "MULTI <成功个数> <之后的空闲堆> <之后的 OCRAM 空闲>"，
//                              然后全部 stop()
//   CA <n>                     之后 n 字节为 PEM，后续连接只信任它；"CA 0" 恢复用根证书束。输出 "CA <n>"
//   MEM                        输出 "MEM <空闲堆> <最大空闲块> <OCRAM 空闲>"
// 网络名与密码在 arduino_secrets.h（仓库中为空，只能编译检查）。
#include <WiFi.h>

#include "arduino_secrets.h"

#include "mm.h" // kfree_size、kmax_free_size（底层包 mm 组件）

#define MULTI_MAX 8

static char ca[4096];
static bool use_ca;

static void print_mem(const char *tag)
{
    Serial.print(tag);
    Serial.print(' ');
    Serial.print(kfree_size(0));
    Serial.print(' ');
    Serial.print(kmax_free_size(0));
    Serial.print(' ');
    Serial.println(kfree_size(MM_HEAP_OCRAM_0));
}

static bool open_tls(WiFiSSLClient &c, const char *host, uint16_t port, unsigned long &ms)
{
    c.setCACert(use_ca ? ca : nullptr);
    unsigned long start = millis();
    bool ok = c.connect(host, port);
    ms = millis() - start;
    return ok;
}

static void cmd_get(const char *host, uint16_t port, const char *path)
{
    WiFiSSLClient c;
    unsigned long ms;
    bool ok = open_tls(c, host, port, ms);
    long total = 0;
    uint32_t sum = 0;
    String first;
    if (ok) {
        c.print("GET ");
        c.print(path);
        c.print(" HTTP/1.1\r\nHost: ");
        c.print(host);
        c.print("\r\nConnection: close\r\n\r\n");
        unsigned long start = millis();
        bool line_done = false;
        while (millis() - start < 10000) {
            int b = c.read();
            if (b < 0) {
                if (!c.connected()) {
                    break;
                }
                delay(1);
                continue;
            }
            total++;
            sum += (uint8_t)b;
            if (!line_done) {
                if (b == '\n') {
                    line_done = true;
                } else if (b != '\r') {
                    first += (char)b;
                }
            }
        }
    }
    c.stop();
    Serial.print("RES ");
    Serial.print(ok ? 1 : 0);
    Serial.print(' ');
    Serial.print(ms);
    Serial.print(' ');
    Serial.print(total);
    Serial.print(' ');
    Serial.print(sum);
    Serial.print(' ');
    Serial.println(first);
}

static void cmd_echo(const char *host, uint16_t port, const char *text)
{
    WiFiSSLClient c;
    unsigned long ms;
    bool ok = open_tls(c, host, port, ms);
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
    Serial.print("ECHO ");
    Serial.print(ok ? 1 : 0);
    Serial.print(' ');
    Serial.print(ms);
    Serial.print(' ');
    Serial.println(back);
}

static void cmd_multi(int n, const char *host, uint16_t port)
{
    static WiFiSSLClient cs[MULTI_MAX];
    int ok = 0;
    for (int i = 0; i < n && i < MULTI_MAX; i++) {
        unsigned long ms;
        if (open_tls(cs[i], host, port, ms)) {
            ok++;
        }
    }
    size_t free_after = kfree_size(0);
    size_t ocram_after = kfree_size(MM_HEAP_OCRAM_0);
    for (int i = 0; i < MULTI_MAX; i++) {
        cs[i].stop();
    }
    Serial.print("MULTI ");
    Serial.print(ok);
    Serial.print(' ');
    Serial.print(free_after);
    Serial.print(' ');
    Serial.println(ocram_after);
}

static void cmd_ca(long n)
{
    long got = 0;
    unsigned long last = millis();
    while (got < n && millis() - last < 2000) {
        int b = Serial.read();
        if (b < 0) {
            delay(1);
            continue;
        }
        if (got < (long)sizeof(ca) - 1) {
            ca[got] = (char)b;
        }
        got++;
        last = millis();
    }
    ca[min(got, (long)sizeof(ca) - 1)] = '\0';
    use_ca = n > 0;
    Serial.print("CA ");
    Serial.println(got);
}

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
    print_mem("MEM");
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
        char *cmd = strtok(buf, " ");
        char *a = strtok(nullptr, " ");
        char *b2 = strtok(nullptr, " ");
        char *rest = strtok(nullptr, "");
        if (cmd == nullptr) {
            continue;
        }
        if (strcmp(cmd, "GET") == 0 && a && b2 && rest) {
            cmd_get(a, atoi(b2), rest);
        } else if (strcmp(cmd, "ECHO") == 0 && a && b2 && rest) {
            cmd_echo(a, atoi(b2), rest);
        } else if (strcmp(cmd, "MULTI") == 0 && a && b2 && rest) {
            cmd_multi(atoi(a), b2, atoi(rest));
        } else if (strcmp(cmd, "CA") == 0 && a) {
            cmd_ca(atol(a));
        } else if (strcmp(cmd, "MEM") == 0) {
            print_mem("MEM");
        } else {
            Serial.println("ERR");
        }
    }
}
