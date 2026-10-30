/*
 * Digital I/O maps Arduino pins through the variant table in pin_map.h. Ignore invalid or
 * unavailable pins. OUTPUT_OPENDRAIN is unimplemented; pinMode leaves the pin unchanged.
 */
#include "bflb_gpio.h"
#include "pin_keep.h"
#include "pin_map.h"

#include "Arduino.h"

/**
 * @brief Resolve the chip GPIO peripheral used by digital I/O.
 */
static struct bflb_device_s *gpio_dev(void)
{
    static struct bflb_device_s *dev = bflb_device_get_by_name("gpio");
    return dev;
}

/*
 * Remember digitalWrite output levels by Arduino pin. bflb_gpio_init clears the output bit, so
 * restore it after pinMode(OUTPUT). Like AVR, digitalWrite(HIGH) before pinMode(OUTPUT) ends high,
 * with a short low interval between operations.
 */
static uint32_t out_high[(NUM_DIGITAL_PINS + 31) / 32];

/**
 * @brief Set or clear the selected GPIO output bit.
 */
static void write_level(int gpio, bool high)
{
    if (high) {
        bflb_gpio_set(gpio_dev(), gpio);
    } else {
        bflb_gpio_reset(gpio_dev(), gpio);
    }
}

/**
 * @brief Configure a usable variant pin; ignore unsupported modes and reserved pins.
 */
void pinMode(pin_size_t pin, PinMode mode)
{
    uint32_t cfg;
    int gpio = pin_to_gpio(pin);

    if (gpio < 0) {
        return;
    }
    switch (mode) {
        case OUTPUT:
            /* Enable input too, so digitalRead observes the actual pin level. */
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
    pin_release_keep(gpio); /* Release low-power retention first, as the SDK does for console pins. */
    bflb_gpio_init(gpio_dev(), gpio, cfg);
    if (mode == OUTPUT) {
        write_level(gpio, out_high[pin / 32] & (1u << (pin % 32)));
    }
}

/**
 * @brief Remember and apply the output level for a usable variant pin.
 */
void digitalWrite(pin_size_t pin, PinStatus status)
{
    int gpio = pin_to_gpio(pin);

    if (gpio < 0) {
        return;
    }
    uint32_t bit = 1u << (pin % 32);
    if (status == LOW) {
        __atomic_fetch_and(&out_high[pin / 32], ~bit, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_or(&out_high[pin / 32], bit, __ATOMIC_RELAXED);
    }
    write_level(gpio, status != LOW);
}

/**
 * @brief Read a usable pin's physical level, returning LOW for an invalid mapping.
 */
PinStatus digitalRead(pin_size_t pin)
{
    int gpio = pin_to_gpio(pin);

    if (gpio < 0) {
        return LOW;
    }
    return bflb_gpio_read(gpio_dev(), gpio) ? HIGH : LOW;
}
