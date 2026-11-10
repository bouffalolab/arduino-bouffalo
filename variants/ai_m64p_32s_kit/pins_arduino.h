#pragma once

/* GPIO numbers are Arduino pin numbers. USB pins and bootstrap are reserved. */
#define NUM_DIGITAL_PINS  37
#define NUM_ANALOG_INPUTS 0

/* The V1.0 schematic maps the blue RGB LED channel to GPIO16. */
#define LED_BUILTIN 16

/* UART0 is connected to the onboard CH340; UART1 shares the red/green LEDs. */
#define PIN_SERIAL_TX 34
#define PIN_SERIAL_RX 35
#define PIN_SERIAL1_TX 14
#define PIN_SERIAL1_RX 15

#define SERIAL_PORT_MONITOR Serial
#define SERIAL_PORT_HARDWARE Serial
#define SERIAL_PORT_HARDWARE1 Serial1
#define SERIAL_PORT_HARDWARE_OPEN1 Serial1
