#ifndef Arduino_h
#define Arduino_h

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "event_groups.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "stdlib_noniso.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HIGH 0x1
#define LOW  0x0

#define INPUT          0x0
#define OUTPUT         0x1
#define INPUT_PULLUP   0x2
#define INPUT_PULLDOWN 0x3

#define CHANGE  1
#define RISING  2
#define FALLING 3

#define PI         3.1415926535897932384626433832795
#define HALF_PI    1.5707963267948966192313216916398
#define TWO_PI     6.283185307179586476925286766559
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define RAD_TO_DEG 57.295779513082320876798154814105
#define EULER      2.718281828459045235360287471352

#define LSBFIRST 0
#define MSBFIRST 1

#ifdef abs
#undef abs
#endif

#define abs(x) ((x) > 0 ? (x) : -(x))
#define constrain(value, low, high) \
    ((value) < (low) ? (low) : ((value) > (high) ? (high) : (value)))
#define round(x) ((x) >= 0 ? (long)((x) + 0.5) : (long)((x) - 0.5))
#define radians(degrees) ((degrees) * DEG_TO_RAD)
#define degrees(radians) ((radians) * RAD_TO_DEG)
#define sq(value) ((value) * (value))
#define bit(bit_number) (1UL << (bit_number))

#ifndef _NOP
#define _NOP() __asm__ volatile("nop")
#endif

typedef bool boolean;
typedef uint8_t byte;
typedef unsigned int word;

void init(void);
void initVariant(void);
void setup(void);
void loop(void);
void yield(void);

unsigned long millis(void);
unsigned long micros(void);
void delay(unsigned long milliseconds);
void delayMicroseconds(unsigned int microseconds);

void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t value);
int digitalRead(uint8_t pin);

#ifdef __cplusplus
} // extern "C"

template <typename Left, typename Right>
constexpr auto min(const Left &left, const Right &right) -> decltype(left < right ? left : right)
{
    return left < right ? left : right;
}

template <typename Left, typename Right>
constexpr auto max(const Left &left, const Right &right) -> decltype(left > right ? left : right)
{
    return left > right ? left : right;
}

#include "HardwareSerial.h"
#include "IPAddress.h"
#include "Print.h"
#include "Printable.h"
#include "Server.h"
#include "Stream.h"
#include "WString.h"
#endif

#include "pins_arduino.h"

#endif
