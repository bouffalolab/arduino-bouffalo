#pragma once

/* GPIO numbers are Arduino pin numbers. USB pins and bootstrap are reserved. */
#define NUM_DIGITAL_PINS  37
#define NUM_ANALOG_INPUTS 10

/*
 * Analog inputs: every BL616CL ADC pin on the J1/J2 headers. The ADC channel differs from the
 * GPIO number for A8/A9; channels 10/11 are the reserved USB pins GPIO32/33.
 */
#define PIN_A0 (0u)  /* GPIO0,  ADC channel 0, J2-2 */
#define PIN_A1 (1u)  /* GPIO1,  ADC channel 1, J2-3 */
#define PIN_A2 (2u)  /* GPIO2,  ADC channel 2, J2-16 */
#define PIN_A3 (3u)  /* GPIO3,  ADC channel 3, J1-13 */
#define PIN_A4 (4u)  /* GPIO4,  ADC channel 4, J1-3 */
#define PIN_A5 (5u)  /* GPIO5,  ADC channel 5, J1-4 */
#define PIN_A6 (6u)  /* GPIO6,  ADC channel 6, J2-19 */
#define PIN_A7 (7u)  /* GPIO7,  ADC channel 7, J1-20 */
#define PIN_A8 (12u) /* GPIO12, ADC channel 8, J1-5 */
#define PIN_A9 (13u) /* GPIO13, ADC channel 9, J1-6 */

static const uint8_t A0 = PIN_A0;
static const uint8_t A1 = PIN_A1;
static const uint8_t A2 = PIN_A2;
static const uint8_t A3 = PIN_A3;
static const uint8_t A4 = PIN_A4;
static const uint8_t A5 = PIN_A5;
static const uint8_t A6 = PIN_A6;
static const uint8_t A7 = PIN_A7;
static const uint8_t A8 = PIN_A8;
static const uint8_t A9 = PIN_A9;

/*
 * PWM (analogWrite): GPIO0–GPIO31, all at 1 kHz. Each pin carries the active-high output of PWM0
 * channel GPIO % 4, so pins on the same channel cannot be driven at the same time:
 *   CH0: 0, 4, 8, 12, 16, 20, 24, 28    CH1: 1, 5, 9, 13, 17, 21, 25, 29
 *   CH2: 2, 6, 10, 14, 18, 22, 26, 30   CH3: 3, 7, 11, 15, 19, 23, 27, 31
 * GPIO34/35 (Serial, CH340) and the reserved GPIO32/33/36 are not PWM pins.
 */
#define digitalPinHasPWM(p) ((uint32_t)(p) < 32u)

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
