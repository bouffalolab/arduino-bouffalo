// WiFiServer 检查：连网后在 5558 端口用 available()、在 5559 端口用 accept() 服务，由电脑上的脚本作客户端。
// 5558 按行处理：普通行回大写的原文；"bc" 用 server.write 发 "all\n" 给所有连接；"stop" 回 "bye\n" 后 stop()。
// 5559：accept() 到的连接先收到 "hi\n"，回一行后被 stop()。经 USB 串口输出 "IP <地址>"、"READY"，
// 之后每处理一行输出 "LINE <端口> <内容>"。网络名与密码在 arduino_secrets.h（仓库中为空，只能编译检查）。
#include <WiFi.h>

#include "arduino_secrets.h"

WiFiServer server(5558);
WiFiServer acceptor(5559);

// 读一行（不含 '\n'），最多等 1 s
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
