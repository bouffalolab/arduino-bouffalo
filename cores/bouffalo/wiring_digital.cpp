/*
 * 数字 IO。引脚号即 BL616CL 的 GPIO 号，有效范围由 variant 的 NUM_DIGITAL_PINS 给出；
 * 越界的引脚号被忽略。OUTPUT_OPENDRAIN 尚未实现，pinMode 遇到它时不改动引脚。
 */
#include "bflb_gpio.h"
#include "pin_keep.h"

#include "Arduino.h"

static struct bflb_device_s *gpio_dev(void)
{
    static struct bflb_device_s *dev = bflb_device_get_by_name("gpio");
    return dev;
}

/*
 * digitalWrite 写过的输出电平。bflb_gpio_init 会把输出位清零，pinMode(OUTPUT) 之后按它恢复，
 * 与 AVR 一致：先 digitalWrite(HIGH) 再 pinMode(OUTPUT) 得到高电平（两步之间有短暂低电平）。
 */
static uint32_t out_high[(NUM_DIGITAL_PINS + 31) / 32];

static void write_level(pin_size_t pin, bool high)
{
    if (high) {
        bflb_gpio_set(gpio_dev(), pin);
    } else {
        bflb_gpio_reset(gpio_dev(), pin);
    }
}

void pinMode(pin_size_t pin, PinMode mode)
{
    uint32_t cfg;

    if (pin >= NUM_DIGITAL_PINS) {
        return;
    }
    switch (mode) {
        case OUTPUT:
            /* 同时打开输入，digitalRead 读到的是引脚实际电平 */
            cfg = GPIO_OUTPUT | GPIO_INPUT | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_1;
            break;
        case INPUT_PULLUP:
            cfg = GPIO_INPUT | GPIO_PULLUP | GPIO_SMT_EN;
            break;
        case INPUT_PULLDOWN:
            cfg = GPIO_INPUT | GPIO_PULLDOWN | GPIO_SMT_EN;
            break;
        case INPUT:
            cfg = GPIO_INPUT | GPIO_FLOAT | GPIO_SMT_EN;
            break;
        default:
            return;
    }
    pin_release_keep(pin); /* 与 SDK 配置控制台引脚时相同，先解除低功耗保持 */
    bflb_gpio_init(gpio_dev(), pin, cfg);
    if (mode == OUTPUT) {
        write_level(pin, out_high[pin / 32] & (1u << (pin % 32)));
    }
}

void digitalWrite(pin_size_t pin, PinStatus status)
{
    if (pin >= NUM_DIGITAL_PINS) {
        return;
    }
    uint32_t bit = 1u << (pin % 32);
    if (status == LOW) {
        __atomic_fetch_and(&out_high[pin / 32], ~bit, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_or(&out_high[pin / 32], bit, __ATOMIC_RELAXED);
    }
    write_level(pin, status != LOW);
}

PinStatus digitalRead(pin_size_t pin)
{
    if (pin >= NUM_DIGITAL_PINS) {
        return LOW;
    }
    return bflb_gpio_read(gpio_dev(), pin) ? HIGH : LOW;
}
