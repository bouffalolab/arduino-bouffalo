#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include <stdint.h>

/*
 * Temporary UNO R4 WiFi BL616CL mapping.
 *
 * The SDK BSP remains bl616cldk while the carrier schematic is unfinished.
 * Only the UART assignments below are suitable for build/profile bring-up.
 * Replace this file and the bridge_config.h provisional values together when
 * the carrier signal table is frozen.
 */
#define NUM_DIGITAL_PINS 37U
#define NUM_ANALOG_INPUTS 12U

static const int8_t PIN_NOT_CONNECTED = -1;

static const uint8_t LED_BUILTIN = 31;
#define BUILTIN_LED LED_BUILTIN

/* Provisional DK UART routing used only until the UNO R4 carrier is defined. */
static const uint8_t PIN_SERIAL_TX = 34;
static const uint8_t PIN_SERIAL_RX = 35;
static const uint8_t PIN_SERIAL1_TX = 24;
static const uint8_t PIN_SERIAL1_RX = 25;

static const uint8_t TX = PIN_SERIAL_TX;
static const uint8_t RX = PIN_SERIAL_RX;
static const uint8_t TX1 = PIN_SERIAL1_TX;
static const uint8_t RX1 = PIN_SERIAL1_RX;

/* BL616CL DK USB pads; final hardware must confirm this fixed USB routing. */
static const uint8_t PIN_USB_DM = 32;
static const uint8_t PIN_USB_DP = 33;

static const uint8_t SDA = 11;
static const uint8_t SCL = 14;

#define ARDUINO_LOOP_STACK_SIZE 2048U
#define ARDUINO_LOOP_PRIORITY   1U

#endif
