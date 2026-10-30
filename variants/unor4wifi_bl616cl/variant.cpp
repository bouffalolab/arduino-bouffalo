#include "Arduino.h"
#include "pin_map.h"

/*
 * Arduino pin numbers equal GPIO numbers. Reserve GPIO4/5 for the 32 kHz crystal, GPIO32/33 for
 * USB and GPIO36 for bootstrap (high on reset enters ROM). These assignments match 1V0/1V1;
 * internal Flash uses a dedicated interface, not GPIOs.
 */
static constexpr uint8_t NA = PIN_MAP_UNUSED;

const uint8_t g_pin_map[] = {
    0,  1,  2,  3,  NA, NA, 6,  7,  8,  9,  /*  0– 9 */
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19, /* 10–19 */
    20, 21, 22, 23, 24, 25, 26, 27, 28, 29, /* 20–29 */
    30, 31, NA, NA, 34, 35, NA,             /* 30–36 */
};
static_assert(sizeof(g_pin_map) == NUM_DIGITAL_PINS, "g_pin_map must cover pins 0 to NUM_DIGITAL_PINS - 1");

Uart Serial1(0, PIN_SERIAL1_TX, PIN_SERIAL1_RX);
Uart Serial2(1, PIN_SERIAL2_TX, PIN_SERIAL2_RX);
