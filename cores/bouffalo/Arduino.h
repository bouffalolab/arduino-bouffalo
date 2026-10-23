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

#ifdef __cplusplus
extern "C" {
#endif
/* BL616CL 软件复位进入 ROM 下载模式（UART 与 USB），不等待未发出的串口数据，不返回；实现在 isp_boot.c */
void resetToBootloader(void) __attribute__((noreturn));
#ifdef __cplusplus
}
#endif

#if defined(__cplusplus)
#include "USBSerial.h"
#include "Uart.h"
#endif

#endif
