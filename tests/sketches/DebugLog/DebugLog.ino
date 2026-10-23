// 调试日志检查：每 500 ms 用 printf 输出 "LOG <n>"，用 Serial（USB）输出 "USB <n>"。
// 板卡菜单 Debug log 打开时 LOG 行只出现在 uart2（J6 IO41），关闭时被丢弃；两种情况下 USB 上都只有 USB 行
#include <stdio.h>

void setup()
{
    Serial.begin(115200);
}

void loop()
{
    static unsigned long n;
    printf("LOG %lu\r\n", n);
    Serial.print("USB ");
    Serial.println(n);
    n++;
    delay(500);
}
