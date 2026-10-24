// WiFi 扫描检查：未连接时扫描 3 次，结果经 USB 串口输出。每次一行 "SCAN <个数> <耗时ms>"，
// 再每个网络一行 "AP <序号>|<ssid>|<bssid>|<rssi>|<信道>|<加密>"；之后一行越界序号的结果
// "OOR <ssid 长度> <rssi> <信道> <加密>"，最后一行 "DONE"。
#include <WiFi.h>

static void scan()
{
    unsigned long start = millis();
    int n = WiFi.scanNetworks();
    unsigned long took = millis() - start;
    Serial.print("SCAN ");
    Serial.print(n);
    Serial.print(' ');
    Serial.println(took);
    for (int i = 0; i < n; i++) {
        uint8_t b[WL_MAC_ADDR_LENGTH];
        char row[96];
        WiFi.BSSID(i, b);
        snprintf(row, sizeof(row), "AP %d|%s|%02X:%02X:%02X:%02X:%02X:%02X|%ld|%u|%u", i, WiFi.SSID(i), b[0], b[1],
                 b[2], b[3], b[4], b[5], (long)WiFi.RSSI(i), WiFi.channel(i), WiFi.encryptionType(i));
        Serial.println(row);
    }
}

void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 10000) {
        delay(10);
    }
    for (int k = 0; k < 3; k++) {
        scan();
    }
    Serial.print("OOR ");
    Serial.print(strlen(WiFi.SSID(WL_NETWORKS_LIST_MAXNUM)));
    Serial.print(' ');
    Serial.print(WiFi.RSSI(WL_NETWORKS_LIST_MAXNUM));
    Serial.print(' ');
    Serial.print(WiFi.channel(WL_NETWORKS_LIST_MAXNUM));
    Serial.print(' ');
    Serial.println(WiFi.encryptionType(WL_NETWORKS_LIST_MAXNUM));
    Serial.println("DONE");
}

void loop()
{
}
