/*
 * Arduino API entry point. Generic interfaces come from the unmodified ArduinoCore-API in api/
 * (see README.md). Chip implementations live here; board differences belong in variants/.
 */
#ifndef Arduino_h
#define Arduino_h

#include "api/ArduinoAPI.h"

#if defined(__cplusplus)
using namespace arduino;
#endif

#include "pins_arduino.h"

/* analogReference() mode: the GPADC internal reference, 3.2 V single-ended full scale. */
#define AR_DEFAULT 0

#ifdef __cplusplus
extern "C" {
#endif
/**
 * @brief Reset BL616CL into ROM download mode over UART or USB. Pending serial output is not
 * drained. Does not return; implemented in isp_boot.c.
 */
void resetToBootloader(void) __attribute__((noreturn));

/**
 * @brief Set the analogRead() result width in bits (1–31, default 10). Above the 12 effective
 * bits the value is padded with zero low bits. Implemented in wiring_analog.cpp.
 */
void analogReadResolution(int bits);

/**
 * @brief Set the analogWrite() value width in bits (1–31, default 8). The default 1 kHz PWM has
 * 40000 steps, so wider values lose their extra low bits. Implemented in wiring_analog.cpp.
 */
void analogWriteResolution(int bits);

/**
 * @brief Platform extension, not in the Arduino API (same form as STM32duino): set the PWM
 * frequency of every analogWrite() pin to 1 Hz–1 MHz, default 1 kHz; other values are ignored.
 * The 40 MHz clock is divided by the smallest d that keeps a period within 65535 counts, so a
 * period has N = round(40 MHz / (d × hz)) duty steps (40000 at 1 kHz, 40 at 1 MHz) and the real
 * frequency is 40 MHz / (d × N). All PWM pins share one counter. The call waits for the current
 * period to end (up to about 1 s at 1 Hz), then the counter restarts and running pins keep their
 * duty. Implemented in wiring_analog.cpp.
 */
void analogWriteFrequency(uint32_t hz);
#ifdef __cplusplus
}
#endif

#if defined(__cplusplus)
#include "USBSerial.h"
#include "Uart.h"
#endif

#endif
