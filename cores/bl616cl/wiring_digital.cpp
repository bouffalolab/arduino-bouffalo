#include "Arduino.h"

#include "bflb_gpio.h"

static struct bflb_device_s *gpio_device(void)
{
    static struct bflb_device_s *device;
    if (device == NULL) {
        device = bflb_device_get_by_name("gpio");
    }
    return device;
}

static bool valid_pin(uint8_t pin)
{
    return pin < NUM_DIGITAL_PINS;
}

extern "C" void pinMode(uint8_t pin, uint8_t mode)
{
    struct bflb_device_s *gpio = gpio_device();
    if ((gpio == NULL) || !valid_pin(pin)) {
        return;
    }

    uint32_t config;
    switch (mode) {
        case INPUT:
            config = GPIO_INPUT | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_1;
            break;
        case INPUT_PULLUP:
            config = GPIO_INPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1;
            break;
        case INPUT_PULLDOWN:
            config = GPIO_INPUT | GPIO_PULLDOWN | GPIO_SMT_EN | GPIO_DRV_1;
            break;
        case OUTPUT:
        default:
            config = GPIO_OUTPUT | GPIO_PULLDOWN | GPIO_SMT_EN | GPIO_DRV_1;
            break;
    }

    bflb_gpio_init(gpio, pin, config);
}

extern "C" void digitalWrite(uint8_t pin, uint8_t value)
{
    struct bflb_device_s *gpio = gpio_device();
    if ((gpio == NULL) || !valid_pin(pin)) {
        return;
    }

    if (value == LOW) {
        bflb_gpio_reset(gpio, pin);
    } else {
        bflb_gpio_set(gpio, pin);
    }
}

extern "C" int digitalRead(uint8_t pin)
{
    struct bflb_device_s *gpio = gpio_device();
    if ((gpio == NULL) || !valid_pin(pin)) {
        return LOW;
    }
    return bflb_gpio_read(gpio, pin) ? HIGH : LOW;
}
