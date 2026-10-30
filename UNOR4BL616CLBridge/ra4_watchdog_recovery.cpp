#include "ra4_watchdog_recovery.h"

#include "Arduino.h"
#include "bridge_config.h"
#include "ra4_bootstrap_control.h"
#include "wifi_ap_start_diagnostic.h"

#include <string.h>

namespace bridge {
namespace {

constexpr uint32_t kSambaBaud = 230400U;
constexpr uint32_t kSambaReadTimeoutMs = 1000U;
constexpr uint32_t kApplicationSettleMs = 100U;

bool wait_for_bytes(
    HardwareSerial &serial,
    const uint8_t *expected,
    size_t expected_size)
{
    size_t matched = 0U;
    const uint32_t deadline = millis() + kSambaReadTimeoutMs;
    while (matched < expected_size &&
           static_cast<int32_t>(millis() - deadline) < 0) {
        const int value = serial.read();
        if (value < 0) {
            delay(1);
            continue;
        }
        if (static_cast<uint8_t>(value) == expected[matched]) {
            ++matched;
        } else {
            matched = static_cast<uint8_t>(
                static_cast<uint8_t>(value) == expected[0] ? 1U : 0U);
        }
    }
    return matched == expected_size;
}

bool send_byte(HardwareSerial &serial, uint8_t value)
{
    return serial.write(value) == 1U;
}

bool send_bytes(HardwareSerial &serial, const uint8_t *data, size_t size)
{
    return serial.write(data, size) == size;
}

bool probe_samba(HardwareSerial &serial)
{
    static constexpr uint8_t kAutoBaudByte = 0x80U;
    static constexpr uint8_t kAutoBaudConfirm = '#';
    static constexpr uint8_t kBinaryMode[] = {'N', '#'};
    static constexpr uint8_t kBinaryReply[] = {'\n', '\r'};
    static constexpr uint8_t kVersionRequest[] = {'V', '#'};
    static constexpr uint8_t kRunRequest[] = {'K', '#'};

    serial.clearRx();
    if (!send_byte(serial, kAutoBaudByte)) {
        return false;
    }
    delay(10);
    if (!send_byte(serial, kAutoBaudByte)) {
        return false;
    }
    delay(10);
    if (!send_byte(serial, kAutoBaudConfirm)) {
        return false;
    }
    delay(10);

    serial.clearRx();
    if (!send_bytes(serial, kBinaryMode, sizeof(kBinaryMode)) ||
        !wait_for_bytes(serial, kBinaryReply, sizeof(kBinaryReply))) {
        return false;
    }

    serial.clearRx();
    if (!send_bytes(serial, kVersionRequest, sizeof(kVersionRequest))) {
        return false;
    }
    const uint32_t version_deadline = millis() + kSambaReadTimeoutMs;
    char version[96] = {};
    size_t version_size = 0U;
    while (static_cast<int32_t>(millis() - version_deadline) < 0 &&
           version_size + 1U < sizeof(version)) {
        const int value = serial.read();
        if (value < 0) {
            delay(1);
            continue;
        }
        version[version_size++] = static_cast<char>(value);
        if (version_size >= 2U &&
            version[version_size - 2U] == '\r' &&
            version[version_size - 1U] == '\n') {
            break;
        }
    }
    if (strstr(version, "Arduino Bootloader (SAM-BA extended) ") == nullptr) {
        return false;
    }

    serial.clearRx();
    return send_bytes(serial, kRunRequest, sizeof(kRunRequest));
}

} // namespace

bool ra4_watchdog_recover_application(
    Ra4BootstrapControl &bootstrap,
    HardwareSerial &serial)
{
#if defined(BL616CL_AP_START_DIAGNOSTIC_WATCHDOG) && \
    BL616CL_AP_START_DIAGNOSTIC_WATCHDOG
    if (!wifi_ap_start_watchdog_recovery_required()) {
        return true;
    }

    if (!bootstrap.enter_program_mode() ||
        !serial.updateBaudRate(kSambaBaud)) {
        return false;
    }
    delay(kApplicationSettleMs);

    const bool samba_ok = probe_samba(serial);
    if (!samba_ok) {
        (void)serial.updateBaudRate(kDefaultUserUartBaud);
        (void)bootstrap.release();
        return false;
    }

    delay(50U);
    const bool restored_baud = serial.updateBaudRate(kDefaultUserUartBaud);
    const bool released = bootstrap.release();
    if (restored_baud && released) {
        wifi_ap_start_watchdog_recovery_complete();
    }
    return restored_baud && released;
#else
    (void)bootstrap;
    (void)serial;
    return true;
#endif
}

} // namespace bridge
