#include <Arduino.h>
#include "driver/gpio.h"

#include "bflb_gpio.h"

/*
 * ESP32-style GPIO shim on top of the BL616CL lhal.
 *
 * BL616CL's cfgset gives GPIO_INPUT and GPIO_OUTPUT independent enable bits,
 * so GPIO_MODE_INPUT_OUTPUT is a real bidirectional pad: configure it once,
 * then drive and sample without any per-transfer direction change.  The chip
 * has no open-drain cfgset bit, so the *_OD modes degrade to their push-pull
 * counterparts.
 */

static struct bflb_device_s *gpio_device(void)
{
    static struct bflb_device_s *device;
    if (device == NULL) {
        device = bflb_device_get_by_name("gpio");
    }
    return device;
}

/* Plain INPUT/OUTPUT keep the exact flag sets pinMode() uses, so existing
 * callers see no electrical change. */
static uint32_t cfgset_for(gpio_mode_t mode)
{
    switch (mode) {
        case GPIO_MODE_OUTPUT:
        case GPIO_MODE_OUTPUT_OD:       /* no open-drain pad on BL616CL */
            return GPIO_OUTPUT | GPIO_PULLDOWN | GPIO_SMT_EN | GPIO_DRV_1;

        case GPIO_MODE_INPUT_OUTPUT:
        case GPIO_MODE_INPUT_OUTPUT_OD: /* no open-drain pad on BL616CL */
            return GPIO_INPUT | GPIO_OUTPUT | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_1;

        case GPIO_MODE_DISABLE:
        case GPIO_MODE_INPUT:
        default:
            return GPIO_INPUT | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_1;
    }
}

extern "C" void gpio_config(gpio_config_t *cfg)
{
    if (cfg == nullptr) {
        return;
    }

    struct bflb_device_s *gpio = gpio_device();
    if (gpio == NULL) {
        return;
    }

    for (int pin = 0; pin < 64; ++pin) {
        if ((cfg->pin_bit_mask & (1ULL << pin)) == 0) {
            continue;
        }
        if (pin >= (int)NUM_DIGITAL_PINS) {
            continue;
        }
        bflb_gpio_init(gpio, static_cast<uint8_t>(pin), cfgset_for(cfg->mode));
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
    struct bflb_device_s *device = gpio_device();
    if (device == NULL) {
        return;
    }
    if (gpio < 0 || gpio >= (int)NUM_DIGITAL_PINS) {
        return;
    }
    bflb_gpio_init(device, static_cast<uint8_t>(gpio), cfgset_for(mode));
}
