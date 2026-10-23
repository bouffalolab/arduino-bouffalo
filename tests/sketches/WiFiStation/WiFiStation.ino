// WiFi 库检查：连接、地址查询、重复 begin、ping 网关 20 次、DNS、TCP（网关 80 端口）、错误密码、
// 不存在的 SSID、断开与重连。结果经 USB 串口逐行输出 "<项> <值>..."，最后一行 "DONE"。
// 网络名与密码在 arduino_secrets.h（仓库中为空，只能编译检查）。
#include <WiFi.h>

#include "arduino_secrets.h"

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

static void addr(const char *item, IPAddress ip)
{
    Serial.print(item);
    Serial.print(' ');
    Serial.println(ip);
}

// begin 超时后连接仍在后台进行，等它出结果
static int settle(int st)
{
    unsigned long start = millis();
    while (st == WL_IDLE_STATUS && millis() - start < 20000) {
        delay(100);
        st = WiFi.status();
    }
    return st;
}

// begin 并输出 "<项> 状态 原因码 耗时ms"
static int connect(const char *item, const char *ssid, const char *pass)
{
    unsigned long start = millis();
    int st = WiFi.begin(ssid, pass);
    unsigned long took = millis() - start;
    st = settle(st);
    line(item, st, WiFi.reasonCode(), took);
    return st;
}

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

    // 连接进行中调用 disconnect()：连接比断开的等待（3 s）更晚完成时，也不能变成已连接；之后能正常再连。
    // 输出 "DISCMID <15 s 后状态> <期间是否出现过 WL_CONNECTED> <disconnect 耗时ms>"
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

void loop()
{
    delay(5000);
    line("ALIVE", WiFi.status(), WiFi.RSSI(), millis());
}
