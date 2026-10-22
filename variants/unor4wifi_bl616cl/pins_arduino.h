#pragma once

/*
 * UNO R4 WiFi，协处理器 ESP32-S3-MINI-1 被 BL616CL 模组按焊盘替换。
 * 这里只描述 BL616CL 一侧；UNO 排针上的 D0–D13、A0–A5 属于 RA4M1，不在本 variant 中。
 *
 * 引脚号即 BL616CL 的 GPIO 号。下列落点只有 UART0（GPIO34 发、GPIO35 收）与 BL616CL
 * 数据手册的固定功能一致；替换模组的焊盘是否真的接到这些 GPIO，仍待模组资料或实测确认。
 * 连到 RA4M1 的 UART1、RESET、MD、SWD 等引脚待确认后再加入。
 */

#define NUM_DIGITAL_PINS  37 /* GPIO0–GPIO36 */
#define NUM_ANALOG_INPUTS 0

/*
 * 板上 RX LED（DL2，经 330 Ω 接 3.3 V）挂在协处理器的 TXD0 上，低电平点亮，
 * 与 Serial 的 TX 是同一个引脚：Blink 中 digitalWrite(LED_BUILTIN, LOW) 时灯亮，
 * Serial.begin() 之后该引脚归 UART 使用。
 */
#define LED_BUILTIN 34

/* Serial：uart0，经 J6 的 TXD0/RXD0 引出，同时连到 RA4M1 的 SCI9（经电平转换） */
#define SERIAL_UART_INDEX 0
#define PIN_SERIAL_TX     34
#define PIN_SERIAL_RX     35
