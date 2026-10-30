#pragma once

/*
 * Map Arduino pin numbers to GPIOs through variant g_pin_map. The Core uses pin_to_gpio rather
 * than reading the table directly. Future ADC/PWM metadata belongs here and in each variant.
 */
#include <stdint.h>

#include "api/Common.h"
#include "pins_arduino.h"

/* Table value for a reserved or unexposed Arduino pin. */
#define PIN_MAP_UNUSED 0xFF

#ifdef __cplusplus
extern "C" {
#endif

/* Indexed by Arduino pin number; NUM_DIGITAL_PINS entries, checked by the variant at compile time. */
extern const uint8_t g_pin_map[];

#ifdef __cplusplus
}
#endif

/**
 * @brief Return the mapped GPIO, or -1 for an invalid or unavailable pin.
 */
static inline int pin_to_gpio(pin_size_t pin)
{
    if (pin >= NUM_DIGITAL_PINS || g_pin_map[pin] == PIN_MAP_UNUSED) {
        return -1;
    }
    return g_pin_map[pin];
}
