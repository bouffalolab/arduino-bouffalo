#pragma once

/*
 * UNO R4 WiFi，协处理器 ESP32-S3-MINI-1 被 BL616CL 模组按焊盘替换。
 * 这里只描述 BL616CL 一侧；UNO 排针上的 D0–D13、A0–A5 属于 RA4M1，不在本 variant 中。
 *
 * 引脚号即 BL616CL 的 GPIO 号，可用的号见 variant.cpp 的引脚表。落点取自模组引脚对照表（docs/reference/uno-r4-wifi.md
 * “板内连接”）：焊盘 39/40（ESP_TXD0/RXD0）接 GPIO34/35，两版模组相同。
 * 连到 RA4M1 的 RESET、MD、SWD 属于 bridge 应用，不在本 variant 中。
 */

#define NUM_DIGITAL_PINS  37 /* GPIO0–GPIO36 */
#define NUM_ANALOG_INPUTS 0

/*
 * 板上 RX LED（DL2，经 330 Ω 接 3.3 V）挂在协处理器的 TXD0 上，低电平点亮，
 * 与 Serial1 的 TX 是同一个引脚：Blink 中 digitalWrite(LED_BUILTIN, LOW) 时灯亮，
 * Serial1.begin() 之后该引脚归 UART 使用。
 */
#define LED_BUILTIN 34

/*
 * Serial 是 BL616CL 的 USB CDC（电脑能看到的口，USB 身份见 boards.txt）。
 * Serial1：uart0，经 J6 的 TXD0/RXD0 引出，同时连到 RA4M1 的 SCI9（经电平转换）。
 * Serial2：uart1，连到 RA4M1 的 SCI1（AT 通道）。
 */
#define PIN_SERIAL1_TX 34
#define PIN_SERIAL1_RX 35
#define PIN_SERIAL2_TX 6
#define PIN_SERIAL2_RX 7

/*
 * 调试日志（板卡菜单 Debug log 打开时）：uart2，经 J6 的 IO41（发）、IO42（收）引出。
 * 这里是 1V0 模组的 GPIO11、GPIO8；1V1 模组为 GPIO30、GPIO31。打开后这两个引脚归日志使用。
 */
#define PIN_DEBUG_LOG_TX 11
#define PIN_DEBUG_LOG_RX 8

#define SERIAL_PORT_MONITOR     Serial
#define SERIAL_PORT_USBVIRTUAL  Serial
#define SERIAL_PORT_HARDWARE    Serial1
#define SERIAL_PORT_HARDWARE1   Serial2
#define SERIAL_PORT_HARDWARE_OPEN  Serial1
#define SERIAL_PORT_HARDWARE_OPEN1 Serial2
