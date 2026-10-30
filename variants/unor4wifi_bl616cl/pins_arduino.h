#pragma once

/*
 * UNO R4 WiFi with its ESP32-S3-MINI-1 replaced pad-for-pad by BL616CL. This variant describes
 * only BL616CL; header D0-D13/A0-A5 belong to RA4M1. Arduino pin numbers equal GPIO numbers; see
 * variant.cpp for availability. The module mapping assigns pads 39/40 (ESP_TXD0/RXD0) to GPIO34/35
 * on both module versions. RA4M1 RESET/MD/SWD controls belong to the bridge application, not this
 * variant.
 */

#define NUM_DIGITAL_PINS  37 /* GPIO0–GPIO36 */
#define NUM_ANALOG_INPUTS 0

/*
 * Active-low RX LED DL2 connects through 330 ohms to 3.3 V on coprocessor TXD0. It shares Serial1
 * TX: digitalWrite(LED_BUILTIN, LOW) lights it until Serial1.begin() takes over the pin.
 */
#define LED_BUILTIN 34

/*
 * Serial is BL616CL native USB CDC (identity in boards.txt). Serial1 is uart0 on J6 TXD0/RXD0,
 * also connected through level shifting to RA4M1 SCI9. Serial2 is uart1 connected to RA4M1 SCI1
 * for AT.
 */
#define PIN_SERIAL1_TX 34
#define PIN_SERIAL1_RX 35
#define PIN_SERIAL2_TX 6
#define PIN_SERIAL2_RX 7

/*
 * Debug log selects uart2 on J6 IO41 TX/IO42 RX. These are GPIO11/8 on 1V0, GPIO30/31 on 1V1.
 * Enabling logging reserves both pins.
 */
#define PIN_DEBUG_LOG_TX 11
#define PIN_DEBUG_LOG_RX 8

#define SERIAL_PORT_MONITOR     Serial
#define SERIAL_PORT_USBVIRTUAL  Serial
#define SERIAL_PORT_HARDWARE    Serial1
#define SERIAL_PORT_HARDWARE1   Serial2
#define SERIAL_PORT_HARDWARE_OPEN  Serial1
#define SERIAL_PORT_HARDWARE_OPEN1 Serial2
