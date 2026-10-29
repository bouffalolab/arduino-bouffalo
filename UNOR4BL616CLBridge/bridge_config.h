#ifndef UNOR4_BL616CL_BRIDGE_CONFIG_H
#define UNOR4_BL616CL_BRIDGE_CONFIG_H

#include <cstddef>
#include <stdint.h>

#include "ra4_pin_config.h"

namespace bridge {

constexpr int8_t kPinUnassigned = -1;

/*
 * Current provisional carrier contract from Ardunio_R4_WiFi_BL616CL.xlsx.
 * Electrical polarity, boot/recovery timing, and USB enumeration remain
 * hardware-validation gates. Keep all carrier-dependent signals here so a
 * final schematic only changes this board contract and the matching Arduino
 * variant/DTS, not service code.
 */
constexpr uint8_t kRa4UserUartTx = PIN_SERIAL_TX;
constexpr uint8_t kRa4UserUartRx = PIN_SERIAL_RX;
constexpr uint8_t kRa4AtUartTx = PIN_SERIAL1_TX;
constexpr uint8_t kRa4AtUartRx = PIN_SERIAL1_RX;
constexpr uint8_t kUsbDm = PIN_USB_DM;
constexpr uint8_t kUsbDp = PIN_USB_DP;

constexpr uint8_t kRa4Boot = UNOR4_BL616CL_RA4_BOOT_PIN;
constexpr uint8_t kRa4Reset = UNOR4_BL616CL_RA4_RESET_PIN;
constexpr uint8_t kRa4SwdIo = UNOR4_BL616CL_RA4_SWDIO_PIN;
constexpr uint8_t kRa4SwdClk = UNOR4_BL616CL_RA4_SWCLK_PIN;

constexpr uint8_t kBl616clDownloadStrap = 36U;
/* Final carrier: FTDI RX is connected to the BL616CL log TX on GPIO8. */
constexpr uint8_t kBl616clConsoleTx = 8U;
constexpr uint8_t kBl616clJtagTdi = 11U;

constexpr unsigned long kDefaultUserUartBaud = 115200UL;
constexpr unsigned long kDefaultAtUartBaud = 115200UL;

/*
 * The current BL616CL bundle keeps the controller coexistence bit disabled
 * unless it is set before fhost_init().
 */
constexpr bool kEnableWifiBleCoexistence = true;

/* Automatic STA PS-PTA failed the connected TLS/GATT gate. Keep the base
 * coexistence bit enabled and make that SDK path opt-in for diagnosis until
 * its scheduling passes the same gate.
 */
#if defined(BL616CL_DIAGNOSTIC_ENABLE_STA_PS_PTA)
constexpr bool kEnableStaPsPtaCoexistence = true;
#else
constexpr bool kEnableStaPsPtaCoexistence = false;
#endif

#ifndef BL616CL_FIRMWARE_PATCH
#define BL616CL_FIRMWARE_PATCH 0
#endif
#define BL616CL_VERSION_STRING_IMPL(value) #value
#define BL616CL_VERSION_STRING(value) BL616CL_VERSION_STRING_IMPL(value)
constexpr char kFirmwareVersion[] = "0.6." BL616CL_VERSION_STRING(BL616CL_FIRMWARE_PATCH);
constexpr uint8_t kFirmwareMajor = 0;
constexpr uint8_t kFirmwareMinor = 6;
constexpr uint8_t kFirmwarePatch = BL616CL_FIRMWARE_PATCH;
#undef BL616CL_VERSION_STRING
#undef BL616CL_VERSION_STRING_IMPL

/*
 * These are protocol limits, not carrier-specific values. Keep them here so
 * AT handlers have one documented contract while the transport moves from its
 * interrupt/ring-buffer backend to a DMA backend after hardware validation.
 */
constexpr size_t kAtMaxLineBytes = 1024U;
constexpr size_t kAtMaxRawPayloadBytes = 16384U;
constexpr size_t kAtMaxResponseBytes = kAtMaxRawPayloadBytes + kAtMaxLineBytes + 16U;
constexpr unsigned long kAtRawPayloadTimeoutMs = 5000UL;
constexpr size_t kAtPollBudgetBytes = 256U;
constexpr size_t kAtUartRxBufferBytes = 2048U;
constexpr size_t kAtUartTxBufferBytes = 4096U;

} // namespace bridge

#endif
