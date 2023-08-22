#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include <stdint.h>

static const uint8_t LED_BUILTIN = 32;
#define BUILTIN_LED LED_BUILTIN // backward compatibility
#define LED_BUILTIN LED_BUILTIN

// Peripheral pins numbers
static const uint8_t SDA = 10;
static const uint8_t SCL = 11;

static const uint8_t UART0_TX = 21;
static const uint8_t UART0_RX = 22;
static const uint8_t UART1_TX = 23;
static const uint8_t UART1_RX = 24;

#endif