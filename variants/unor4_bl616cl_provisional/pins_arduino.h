#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include <stdint.h>

/*
 * UNO R4 WiFi BL616CL provisional carrier mapping.
 *
 * Values come from Ardunio_R4_WiFi_BL616CL.xlsx. The SDK BSP still uses
 * bl616cldk, so its console and early board initialization are not yet a
 * final carrier BSP. Keep bridge control pins in bridge_config.h.
 */
#define NUM_DIGITAL_PINS 37U
#define NUM_ANALOG_INPUTS 12U

static const int8_t PIN_NOT_CONNECTED = -1;

static const uint8_t LED_BUILTIN = 31;
#define BUILTIN_LED LED_BUILTIN

/* RA4M1 user UART: BL616CL UART0. */
static const uint8_t PIN_SERIAL_TX = 34;
static const uint8_t PIN_SERIAL_RX = 35;

/* RA4M1 AT UART: BL616CL UART1. */
static const uint8_t PIN_SERIAL1_TX = 6;
static const uint8_t PIN_SERIAL1_RX = 7;

static const uint8_t TX = PIN_SERIAL_TX;
static const uint8_t RX = PIN_SERIAL_RX;
static const uint8_t TX1 = PIN_SERIAL1_TX;
static const uint8_t RX1 = PIN_SERIAL1_RX;

/* BL616CL native USB pads from the candidate carrier signal table. */
static const uint8_t PIN_USB_DM = 33;
static const uint8_t PIN_USB_DP = 32;

static const uint8_t SDA = 11;
static const uint8_t SCL = 14;

#define ARDUINO_LOOP_STACK_SIZE 2048U
#define ARDUINO_LOOP_PRIORITY   1U

#endif
