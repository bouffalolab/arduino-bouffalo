#include "Arduino.h"
// #include "bouffalo_sdk.h"
#include "bflb_gpio.h"
#include "bflb_adc.h"
#include "bflb_dac.h"
#include "pins_arduino.h"
#include "wiring_private.h"

struct bflb_device_s *gpio;
struct bflb_device_s *dac;
struct bflb_device_s *adc;

uint8_t pin_gpio[] = {11, 15, 16, 17, 18, 21, 22, 23, 24,
                      25, 26, 28, 29, 30, 31, 32, 33, 34};
uint8_t pin_dac[] = {2, 3};
uint8_t pin_adc[PIN_ADC_CNT] = {0, 1, 10, 12, 13, 14, 19, 20, 27};

static int is_pin_gpio(uint8_t pin) {
    for (uint32_t i = 0; i < PIN_GPIO_CNT; i++) {
        if (pin == pin_gpio[i]) {
            return 1;
        }
    }
    return 0;
}

static int is_pin_dac(uint8_t pin) {
    for (uint32_t i = 0; i < PIN_DAC_CNT; i++) {
        if (pin == pin_dac[i]) {
            return 1;
        }
    }
    return 0;
}

static int is_pin_adc(uint8_t pin) {
    for (uint32_t i = 0; i < PIN_ADC_CNT; i++) {
        if (pin == pin_adc[i]) {
            return 1;
        }
    }
    return 0;
}

void pinMode(uint8_t pin, uint8_t mode) {
    gpio = bflb_device_get_by_name("gpio");
    if (gpio == NULL) {
        printf("gpio device get fail!\r\n");
        return;
    }
    if (is_pin_gpio(pin)) {
        if (mode == INPUT) {
            bflb_gpio_init(gpio, pin, GPIO_INPUT | GPIO_PULLDOWN | GPIO_SMT_EN | GPIO_DRV_2);
        } else if (mode == INPUT_PULLUP) {
            bflb_gpio_init(gpio, pin, GPIO_INPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_2);
        } else if (mode == OUTPUT) {
            bflb_gpio_init(gpio, pin, GPIO_OUTPUT | GPIO_PULLDOWN | GPIO_SMT_EN | GPIO_DRV_2);
        }
    } else if (is_pin_dac(pin)) {
        dac = bflb_device_get_by_name("dac");
        if (dac == NULL) {
            printf("dac device get fail!\r\n");
            return;
        }
        bflb_gpio_init(gpio, pin, GPIO_ANALOG | GPIO_SMT_EN | GPIO_DRV_0);
        /* 512K / 16 = 32K */
        bflb_dac_init(dac, DAC_CLK_DIV_16);
        bflb_dac_channel_enable(dac, DAC_CHANNEL_A);
        bflb_dac_channel_enable(dac, DAC_CHANNEL_B);
        bflb_dac_set_value(dac, DAC_CHANNEL_A, 0);
        bflb_dac_set_value(dac, DAC_CHANNEL_B, 0);
    } else if (is_pin_adc(pin)) {
        adc = bflb_device_get_by_name("adc");
        if (adc == NULL) {
            printf("adc device get fail!\r\n");
            return;
        }
        for (uint32_t i = 0; i < PIN_ADC_CNT; i++) {
            bflb_gpio_init(gpio, pin, GPIO_ANALOG | GPIO_SMT_EN | GPIO_DRV_0);
        }
        /* adc clock = XCLK / 2 / 32 */
        struct bflb_adc_config_s cfg;
        cfg.clk_div = ADC_CLK_DIV_32;
        cfg.scan_conv_mode = true;
        cfg.continuous_conv_mode = false;
        cfg.differential_mode = false;
        cfg.resolution = ADC_RESOLUTION_12B;
        cfg.vref = ADC_VREF_3P2V;
        struct bflb_adc_channel_s chan[PIN_ADC_CNT] = {
            {.pos_chan = ADC_CHANNEL_0, .neg_chan = ADC_CHANNEL_GND},
            {.pos_chan = ADC_CHANNEL_1, .neg_chan = ADC_CHANNEL_GND},
            {.pos_chan = ADC_CHANNEL_4, .neg_chan = ADC_CHANNEL_GND},
            {.pos_chan = ADC_CHANNEL_5, .neg_chan = ADC_CHANNEL_GND},
            {.pos_chan = ADC_CHANNEL_6, .neg_chan = ADC_CHANNEL_GND},
            {.pos_chan = ADC_CHANNEL_7, .neg_chan = ADC_CHANNEL_GND},
            {.pos_chan = ADC_CHANNEL_8, .neg_chan = ADC_CHANNEL_GND},
            {.pos_chan = ADC_CHANNEL_9, .neg_chan = ADC_CHANNEL_GND},
            {.pos_chan = ADC_CHANNEL_10, .neg_chan = ADC_CHANNEL_GND},
        };
        bflb_adc_init(adc, &cfg);
        bflb_adc_channel_config(adc, chan, PIN_ADC_CNT);
    } else {
        printf("user should not control this pin, %d!\r\n", pin);
    }
}

void digitalWrite(uint8_t pin, uint8_t val) {
    if (gpio == NULL) {
        printf("need call pinMode function first!\r\n");
        return;
    }
    if (is_pin_gpio(pin) == 0) {
        printf("this pin is not used as a digital pin, %d\n", pin);
        return;
    }
    if (val == LOW) {
        bflb_gpio_reset(gpio, pin);
    } else if (val == HIGH) {
        bflb_gpio_set(gpio, pin);
    }
}

int digitalRead(uint8_t pin) {
    if (gpio == NULL) {
        printf("need call pinMode function first!\r\n");
        return;
    }
    if (is_pin_gpio(pin) == 0) {
        printf("this pin is not used as a digital pin, %d\n", pin);
        return 0;
    }
    if (bflb_gpio_read(gpio, pin)) {
        return 1;
    } else {
        return 0;
    }
}
