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

#ifdef __cplusplus
extern "C" {
#endif
/**
 * @brief Reset BL616CL into ROM download mode over UART or USB. Pending serial output is not
 * drained. Does not return; implemented in isp_boot.c.
 */
void resetToBootloader(void) __attribute__((noreturn));
#ifdef __cplusplus
}
#endif

#if defined(__cplusplus)
#include "USBSerial.h"
#include "Uart.h"
#endif

#endif
