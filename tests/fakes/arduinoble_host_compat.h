#pragma once

#define main arduinoble_unused_embedded_main
#include <Arduino.h>
#undef main

#undef F
#define F(text) text

// Radio debug output has no hardware serial endpoint in these host tests.
struct ArduinoBLEHostSerial {
    template <typename... Args> void print(Args...) {}
    template <typename... Args> void println(Args...) {}
};
static ArduinoBLEHostSerial Serial;
