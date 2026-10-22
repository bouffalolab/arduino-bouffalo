#pragma once

/*
 * 引脚号到 GPIO 的映射。表由 variant 提供（variant.cpp 中的 g_pin_map），Core 只经 pin_to_gpio()
 * 取 GPIO，不直接读表；表以后增加 ADC、PWM 等信息时，只改这里和各 variant。
 */
#include <stdint.h>

#include "api/Common.h"
#include "pins_arduino.h"

/* 表中的这个值表示该引脚号不可用：被板上固定功能占用，或没有引出 */
#define PIN_MAP_UNUSED 0xFF

#ifdef __cplusplus
extern "C" {
#endif

/* 下标是引脚号，长度为 NUM_DIGITAL_PINS（variant 中在编译时核对） */
extern const uint8_t g_pin_map[];

#ifdef __cplusplus
}
#endif

/* 引脚号对应的 GPIO 号；越界或不可用时返回 -1 */
static inline int pin_to_gpio(pin_size_t pin)
{
    if (pin >= NUM_DIGITAL_PINS || g_pin_map[pin] == PIN_MAP_UNUSED) {
        return -1;
    }
    return g_pin_map[pin];
}
