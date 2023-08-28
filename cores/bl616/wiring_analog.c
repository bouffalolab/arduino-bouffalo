#include "Arduino.h"
// #include "bouffalo_sdk.h"
#include "bflb_adc.h"
#include "bflb_gpio.h"
#include "bflb_dac.h"
#include "pins_arduino.h"
#include "wiring_private.h"

static int8_t get_adc_channel(uint8_t pin) {
    int8_t chan = 0;

    switch (pin) {
    case 0:
        chan = 9;
        break;
    case 1:
        chan = 8;
        break;
    case 10:
        chan = 7;
        break;
    case 12:
        chan = 6;
        break;
    case 13:
        chan = 5;
        break;
    case 14:
        chan = 4;
        break;
    case 19:
        chan = 1;
        break;
    case 20:
        chan = 0;
        break;
    case 27:
        chan = 10;
        break;
    default:
        chan = 0;
        break;
    }
    return chan;
}

int analogRead(uint8_t pin)
{
    int ret = 0;
    if (adc == NULL) {
        printf("need call pinMode function first!\r\n");
        return;
    }
    bflb_adc_start_conversion(adc);
    while (bflb_adc_get_count(adc) < PIN_ADC_CNT) {
    }
    for (uint32_t i = 0; i < PIN_ADC_CNT; i++) {
        struct bflb_adc_result_s result;
        uint32_t raw_data = bflb_adc_read_raw(adc);
        bflb_adc_parse_result(adc, &raw_data, &result, 1);
        if (result.pos_chan == get_adc_channel(pin)) {
            ret = result.millivolt;
        }
    }
    return ret;
}

void analogWrite(uint8_t pin, int val)
{
    if (dac == NULL) {
        printf("need call pinMode function first!\r\n");
        return;
    }
    if (val < 0 || val > 4095) {
        printf("analogWrite() function not support this value %d\r\n", val);
        return;
    }
    if (pin == pin_dac[0]) {
        bflb_dac_set_value(dac, DAC_CHANNEL_B, (uint16_t)val);
    } else if (pin == pin_dac[1]) {
        bflb_dac_set_value(dac, DAC_CHANNEL_A, (uint16_t)val);
    } else {
        printf("analogWrite() function not support this pin %d\r\n", pin);
    }
}
