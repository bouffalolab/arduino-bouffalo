#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include <stdint.h>

/*
 * Ai-Thinker Ai-M64L-32S-Kit (BL616CL)
 *
 * Pin definitions follow the Ai-M64L-32S-Kit V1.0 schematic and
 * specification. The Type-C connector is wired to an onboard CH340 USB-UART,
 * so Serial uses UART0 on GPIO34 TX / GPIO35 RX.
 */
#define NUM_DIGITAL_PINS 37U
#define NUM_ANALOG_INPUTS 12U

/* The right-hand cool-white indicator is the default Arduino LED. */
static const uint8_t LED_COOL = 13;
static const uint8_t LED_BUILTIN = LED_COOL;
#define BUILTIN_LED LED_BUILTIN

/* Additional board LEDs. All are active high. */
static const uint8_t LED_WARM = 23;
static const uint8_t LED_RGB_RED = 19;
static const uint8_t LED_RGB_GREEN = 22;
static const uint8_t LED_RGB_BLUE = 18;

static const uint8_t PIN_SERIAL_TX = 34;
static const uint8_t PIN_SERIAL_RX = 35;
static const uint8_t PIN_SERIAL1_TX = 20;
static const uint8_t PIN_SERIAL1_RX = 21;

static const uint8_t TX = PIN_SERIAL_TX;
static const uint8_t RX = PIN_SERIAL_RX;
static const uint8_t TX1 = PIN_SERIAL1_TX;
static const uint8_t RX1 = PIN_SERIAL1_RX;

static const uint8_t SDA = 5;
static const uint8_t SCL = 4;

/* The on-board FLASH button drives the BL616CL BOOT strap. */
static const uint8_t PIN_BUTTON_FLASH = 36;

#define ARDUINO_LOOP_STACK_SIZE 4096U
#define ARDUINO_LOOP_PRIORITY   1U

#endif
