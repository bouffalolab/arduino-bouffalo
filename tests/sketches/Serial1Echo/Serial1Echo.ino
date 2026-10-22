// 回显 Serial1（硬件 UART）收到的字节；每秒打印一次运行时间，用于确认 millis() 与发送
static unsigned long last;

void setup()
{
    Serial1.begin(115200);
    Serial1.println("Serial1Echo ready");
}

void loop()
{
    while (Serial1.available() > 0) {
        Serial1.write(Serial1.read());
    }
    if (millis() - last >= 1000) {
        last = millis();
        Serial1.print("uptime ms: ");
        Serial1.println(last);
    }
}
