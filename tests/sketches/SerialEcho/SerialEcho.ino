// 回显串口收到的字节；每秒打印一次运行时间，用于确认 millis() 与发送
// Serial 是 USB 串口时，等主机打开端口后再输出（官方原生 USB 板卡的写法）
static unsigned long last;

void setup()
{
    Serial.begin(115200);
    while (!Serial) {
    }
    Serial.println("SerialEcho ready");
}

void loop()
{
    while (Serial.available() > 0) {
        Serial.write(Serial.read());
    }
    if (millis() - last >= 1000) {
        last = millis();
        Serial.print("uptime ms: ");
        Serial.println(last);
    }
}
