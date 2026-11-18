#pragma once

/*
 * Map Arduino pin numbers to GPIOs through variant g_pin_map. The Core uses pin_to_gpio rather
 * than reading the table directly. Variants with analog inputs list them in g_analog_pins; the
 * Core maps their GPIOs to ADC channels. Future PWM metadata belongs here and in each variant.
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

#if NUM_ANALOG_INPUTS > 0
/* Arduino pin numbers of A0..A(NUM_ANALOG_INPUTS - 1), checked by the variant at compile time. */
extern const uint8_t g_analog_pins[NUM_ANALOG_INPUTS];
#endif

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
