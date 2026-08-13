#include <Arduino.h>
#include "driver/gpio.h"

extern "C" void gpio_config(gpio_config_t *cfg)
{
    if (cfg == nullptr) {
        return;
    }

    for (int pin = 0; pin < 64; ++pin) {
        if ((cfg->pin_bit_mask & (1ULL << pin)) == 0) {
            continue;
        }
        pinMode(static_cast<uint8_t>(pin),
                cfg->mode == GPIO_MODE_INPUT ? INPUT : OUTPUT);
    }
}

extern "C" void gpio_set_level(int gpio, int level)
{
    digitalWrite(static_cast<uint8_t>(gpio), level ? HIGH : LOW);
}

extern "C" int gpio_get_level(int gpio)
{
    return digitalRead(static_cast<uint8_t>(gpio));
}

extern "C" void gpio_set_direction(int gpio, gpio_mode_t mode)
{
    pinMode(static_cast<uint8_t>(gpio),
            mode == GPIO_MODE_INPUT ? INPUT : OUTPUT);
}
