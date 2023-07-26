#include "Arduino.h"
#include "bouffalo_sdk.h"
#include "pins_arduino.h"

void pinMode(uint8_t ulPin, uint8_t ulMode) {
    struct bflb_device_s *gpio;
    gpio = bflb_device_get_by_name("gpio");
    switch (ulMode)
    {
        if (gpio == NULL) {
            printf("gpio device get fail!\r\n");
            return;
        }
    case INPUT: {
            bflb_gpio_reset(gpio,ulPin);
            bflb_gpio_init(gpio, ulPin, GPIO_INPUT | GPIO_SMT_EN | GPIO_DRV_0);
        }
        break;

    case OUTPUT: {
            bflb_gpio_reset(gpio,ulPin);
            bflb_gpio_init(gpio, ulPin, GPIO_OUTPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_0);
        }
        break;

    case INPUT_PULLUP: {
            bflb_gpio_reset(gpio,ulPin);
            bflb_gpio_init(gpio, ulPin, GPIO_INPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_0);
        }
        break;

    case INPUT_PULLDOWN: {
            bflb_gpio_reset(gpio,ulPin);
            bflb_gpio_init(gpio, ulPin, GPIO_INPUT | GPIO_PULLDOWN | GPIO_SMT_EN | GPIO_DRV_0);
        }
        break;

    default:
        break;
    }
}

void digitalWrite(uint8_t ulPin, uint8_t ulVal) {
#if defined(BL616)
    if (ulPin > 34) {
        printf("ERROR: Illegal pin in pinmode (%d)\r\n", ulPin);
        return;
    }
#endif
    struct bflb_device_s *gpio;
    gpio = bflb_device_get_by_name("gpio");
    if (gpio == NULL) {
        printf("gpio device get fail!\r\n");
        return;
    }
    if (ulVal == HIGH){
        bflb_gpio_set(gpio, ulPin);
    } else {
        bflb_gpio_reset(gpio,ulPin);
    }

}

int digitalRead(uint8_t ulPin) {
    struct bflb_device_s *gpio;
    gpio = bflb_device_get_by_name("gpio");
    if (gpio == NULL) {
        printf("gpio device get fail!\r\n");
        return;
    }
    return bflb_gpio_read(gpio, ulPin);
}