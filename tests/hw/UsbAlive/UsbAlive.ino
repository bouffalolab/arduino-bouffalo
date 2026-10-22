// 实板诊断：经 USB CDC 每秒输出一行，确认固件在运行，并检查 Core 的时间函数与 C++ 全局构造。
// 不驱动任何 GPIO。输出示例：alive 3 millis=3012 delay1000=1001 ctor=1
#include <stdio.h>

#include "usb_cdc.h"

static unsigned ctor_count;

// 全局构造函数应在 setup() 之前执行且只执行一次
struct CtorProbe {
    CtorProbe() { ctor_count++; }
} ctor_probe;

void setup()
{
    usb_cdc_begin();
}

void loop()
{
    static unsigned long count;
    char line[96];
    unsigned long start = millis();
    delay(1000);
    unsigned long elapsed = millis() - start;
    int len = snprintf(line, sizeof(line), "alive %lu millis=%lu delay1000=%lu ctor=%u\r\n",
                       ++count, millis(), elapsed, ctor_count);
    usb_cdc_write(line, (size_t)len);
}
