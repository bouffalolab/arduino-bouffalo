#include "Arduino.h"
#include "pin_map.h"

/* GPIO32/33 are wired to USB and GPIO36 selects the ROM boot mode. */
static constexpr uint8_t NA = PIN_MAP_UNUSED;
const uint8_t g_pin_map[] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
    20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
    30, 31, NA, NA, 34, 35, NA,
};
static_assert(sizeof(g_pin_map) == NUM_DIGITAL_PINS, "g_pin_map must cover all board pins");

const uint8_t g_analog_pins[] = {A0, A1, A2, A3, A4, A5, A6, A7, A8, A9};
static_assert(sizeof(g_analog_pins) == NUM_ANALOG_INPUTS, "g_analog_pins must list A0 to A(n-1)");

Uart Serial(0, PIN_SERIAL_TX, PIN_SERIAL_RX);
Uart Serial1(1, PIN_SERIAL1_TX, PIN_SERIAL1_RX);
