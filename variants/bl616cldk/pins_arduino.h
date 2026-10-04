#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include <stdint.h>

/*
 * BL616CL mapping for the UNO R4 WiFi carrier adaptation.
 *
 * RA4M1 links (through level shifter U4):
 *   UART0 GPIO34 TX / GPIO35 RX -> log + flash UART (SCI9, P109/P110)
 *   UART1 GPIO6 TX / GPIO7 RX   -> AT UART (SCI1, P501/P502)
 *
 * RA4M1 control lines are driven by the bridge firmware:
 *   RESET = GPIO3, MD = GPIO10, SWCLK = GPIO8, SWDIO = GPIO9.
 *
 * UART2/Serial2 is intentionally not mapped: GPIO10 is the RA4M1 MD (boot)
 * line on the carrier, and the core does not instantiate Serial2. Define
 * explicit pins if UART2 is ever wired up.
 */
#define NUM_DIGITAL_PINS 37U
#define NUM_ANALOG_INPUTS 12U

static const uint8_t LED_BUILTIN = 31;
#define BUILTIN_LED LED_BUILTIN

static const uint8_t PIN_SERIAL_TX = 34;
static const uint8_t PIN_SERIAL_RX = 35;
static const uint8_t PIN_SERIAL1_TX = 6;
static const uint8_t PIN_SERIAL1_RX = 7;

static const uint8_t TX = PIN_SERIAL_TX;
static const uint8_t RX = PIN_SERIAL_RX;
static const uint8_t TX1 = PIN_SERIAL1_TX;
static const uint8_t RX1 = PIN_SERIAL1_RX;

static const uint8_t SDA = 11;
static const uint8_t SCL = 14;

#define ARDUINO_LOOP_STACK_SIZE 4096U
#define ARDUINO_LOOP_PRIORITY   1U

#endif
