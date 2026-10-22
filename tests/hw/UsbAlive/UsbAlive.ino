// 实板诊断：经 USB CDC 每秒输出一行，确认固件在运行，并检查 Core 的时间函数、C++ 全局构造和 GPIO 输出。
// LED_BUILTIN 每秒翻转一次，led=写入值/读回的引脚电平；RX LED 低电平点亮。
// 输出示例：alive 3 millis=3012 delay1000=1001 ctor=1 led=0/0
#include <stdio.h>

#include "usb_cdc.h"

static unsigned ctor_count;

// 全局构造函数应在 setup() 之前执行且只执行一次
struct CtorProbe {
    CtorProbe() { ctor_count++; }
} ctor_probe;

void setup()
{
    pinMode(LED_BUILTIN, OUTPUT);
    usb_cdc_begin();
}

void loop()
{
    static unsigned long count;
    char line[96];
    unsigned long start = millis();
    delay(1000);
    unsigned long elapsed = millis() - start;
    PinStatus level = (count % 2) ? HIGH : LOW;
    digitalWrite(LED_BUILTIN, level);
    int len = snprintf(line, sizeof(line), "alive %lu millis=%lu delay1000=%lu ctor=%u led=%d/%d\r\n",
                       ++count, millis(), elapsed, ctor_count, (int)level, (int)digitalRead(LED_BUILTIN));
    usb_cdc_write(line, (size_t)len);
}
