#ifndef UNOR4_BL616CL_HCI_SCAN_ADVERTISING_QUIET_GATE_H
#define UNOR4_BL616CL_HCI_SCAN_ADVERTISING_QUIET_GATE_H

#include <stdint.h>

namespace bridge {

/*
 * ArduinoBLE's AT HCI transport drains packets until the bridge reports no
 * more data. Keep the first advertising reports out of that synchronous
 * command-completion poll so LE Set Scan Enable can return to its caller.
 */
class HciScanAdvertisingQuietGate {
public:
    static constexpr uint32_t kQuietDurationMs = 50U;

    void arm(uint32_t now_ms)
    {
        deadline_ms_ = now_ms + kQuietDurationMs;
        armed_ = true;
    }

    bool should_drop(uint32_t now_ms)
    {
        if (!armed_) {
            return false;
        }
        if (static_cast<int32_t>(now_ms - deadline_ms_) < 0) {
            return true;
        }
        armed_ = false;
        return false;
    }

    void reset()
    {
        armed_ = false;
        deadline_ms_ = 0U;
    }

private:
    bool armed_ = false;
    uint32_t deadline_ms_ = 0U;
};

/*
 * The UNO R4 ArduinoBLE AT transport drains each HCIREAD response synchronously
 * and stores discovered devices in a small dynamic list. Bound report delivery
 * so one poll cannot overrun that consumer on a dense RF channel.
 */
class HciScanAdvertisingRateGate {
public:
    static constexpr uint32_t kMinimumIntervalMs = 50U;

    bool should_drop(uint32_t now_ms)
    {
        if (!armed_) {
            next_delivery_ms_ = now_ms + kMinimumIntervalMs;
            armed_ = true;
            return false;
        }
        if (static_cast<int32_t>(now_ms - next_delivery_ms_) < 0) {
            return true;
        }
        next_delivery_ms_ = now_ms + kMinimumIntervalMs;
        return false;
    }

    void reset()
    {
        armed_ = false;
        next_delivery_ms_ = 0U;
    }

private:
    bool armed_ = false;
    uint32_t next_delivery_ms_ = 0U;
};

} // namespace bridge

#endif
