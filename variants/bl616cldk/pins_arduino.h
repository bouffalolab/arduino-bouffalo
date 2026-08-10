#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include <stdint.h>

/*
 * Stage-1 BL616CL DK mapping. GPIO34/35 are the board console and GPIO32/33
 * are USB D-/D+. The final UNO R4 carrier mapping must replace these values
 * after its schematic is frozen.
 */
#define NUM_DIGITAL_PINS 37U
#define NUM_ANALOG_INPUTS 12U

static const uint8_t LED_BUILTIN = 31;
#define BUILTIN_LED LED_BUILTIN

static const uint8_t PIN_SERIAL_TX = 34;
static const uint8_t PIN_SERIAL_RX = 35;
static const uint8_t PIN_SERIAL1_TX = 24;
static const uint8_t PIN_SERIAL1_RX = 25;

static const uint8_t TX = PIN_SERIAL_TX;
static const uint8_t RX = PIN_SERIAL_RX;
static const uint8_t TX1 = PIN_SERIAL1_TX;
static const uint8_t RX1 = PIN_SERIAL1_RX;

static const uint8_t SDA = 11;
static const uint8_t SCL = 14;

#define ARDUINO_LOOP_STACK_SIZE 1024U
#define ARDUINO_LOOP_PRIORITY   1U

#endif
