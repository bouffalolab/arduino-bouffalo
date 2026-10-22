// 回显串口收到的字节；每秒打印一次运行时间，用于确认 millis() 与发送
static unsigned long last;

void setup()
{
    Serial.begin(115200);
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
