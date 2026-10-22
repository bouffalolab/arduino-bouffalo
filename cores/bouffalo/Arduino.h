/*
 * Arduino API 入口。通用接口来自 ArduinoCore-API（api/，原样复制，见 README.md），
 * BL 芯片的实现在本目录，板卡差异在 variants/。
 */
#ifndef Arduino_h
#define Arduino_h

#include "api/ArduinoAPI.h"

#if defined(__cplusplus)
using namespace arduino;
#endif

#include "pins_arduino.h"

#if defined(__cplusplus)
#include "USBSerial.h"
#include "Uart.h"
#endif

#endif
