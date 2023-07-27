#ifndef WiringPrivate_h
#define WiringPrivate_h


#include <stdio.h>
//#include <stdarg.h>

#include "Arduino.h"
// #include "bflb-hal-timer.h"
// #include "bflb-hal-common.h"
// #include "bflb-hal-gpio.h"

#ifdef __cplusplus
extern "C"{
#endif

#define PIN_GPIO_CNT (18)
#define PIN_DAC_CNT  (2)
#define PIN_ADC_CNT  (9)
extern uint8_t pin_gpio[PIN_GPIO_CNT];
extern uint8_t pin_dac[PIN_DAC_CNT];
extern uint8_t pin_adc[PIN_ADC_CNT];
extern struct bflb_device_s *gpio;
extern struct bflb_device_s *dac;
extern struct bflb_device_s *adc;

#ifdef __cplusplus
} // extern "C"
#endif

#endif
