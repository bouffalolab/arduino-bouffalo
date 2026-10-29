#ifndef UNOR4_BL616CL_HCI_IDLE_SCAN_DISABLE_COMPAT_H
#define UNOR4_BL616CL_HCI_IDLE_SCAN_DISABLE_COMPAT_H

#include <stddef.h>
#include <stdint.h>

namespace bridge {

/*
 * The SDK controller returns Command Disallowed for legacy LE Set Scan Enable
 * with Scan_Enable=0 before a scan activity exists. HCI requires that no-op
 * disable to succeed. ArduinoBLE sends it before Set Scan Parameters.
 */
class HciIdleScanDisableCompat {
public:
    bool should_complete(const uint8_t *packet, size_t packet_size) const
    {
        return !scan_activity_may_exist_ && is_idle_scan_disable(packet, packet_size);
    }

    void note_synthetic_idle_disable()
    {
        synthetic_idle_disable_ = true;
    }

    bool take_scan_parameters_prime_request(const uint8_t *packet, size_t packet_size)
    {
        if (!synthetic_idle_disable_ || !is_legacy_scan_parameters(packet, packet_size)) {
            return false;
        }

        synthetic_idle_disable_ = false;
        return true;
    }

    void observe_host_packet(const uint8_t *packet, size_t packet_size)
    {
        if (!is_h4_command(packet, packet_size)) {
            return;
        }

        const uint16_t opcode = read_le16(packet + 1U);
        if (opcode == kResetOpcode) {
            scan_activity_may_exist_ = false;
            synthetic_idle_disable_ = false;
        } else if (opcode == kLeSetScanParametersOpcode ||
                   opcode == kLeSetExtendedScanParametersOpcode ||
                   (opcode == kLeSetScanEnableOpcode &&
                    packet[3] >= 1U &&
                    packet[4] != 0U)) {
            scan_activity_may_exist_ = true;
        }
    }

    void reset()
    {
        scan_activity_may_exist_ = false;
        synthetic_idle_disable_ = false;
    }

private:
    static constexpr uint8_t kH4Command = 0x01U;
    static constexpr uint16_t kResetOpcode = 0x0C03U;
    static constexpr uint16_t kLeSetScanParametersOpcode = 0x200BU;
    static constexpr uint16_t kLeSetScanEnableOpcode = 0x200CU;
    static constexpr uint16_t kLeSetExtendedScanParametersOpcode = 0x2041U;

    static uint16_t read_le16(const uint8_t *data)
    {
        return static_cast<uint16_t>(data[0]) |
               static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8U);
    }

    static bool is_h4_command(const uint8_t *packet, size_t packet_size)
    {
        return packet != nullptr &&
               packet_size >= 4U &&
               packet[0] == kH4Command &&
               packet_size == static_cast<size_t>(packet[3]) + 4U;
    }

    static bool is_idle_scan_disable(const uint8_t *packet, size_t packet_size)
    {
        return is_h4_command(packet, packet_size) &&
               read_le16(packet + 1U) == kLeSetScanEnableOpcode &&
               packet[3] == 2U &&
               packet[4] == 0U &&
               packet[5] <= 1U;
    }

    static bool is_legacy_scan_parameters(const uint8_t *packet, size_t packet_size)
    {
        return is_h4_command(packet, packet_size) &&
               read_le16(packet + 1U) == kLeSetScanParametersOpcode &&
               packet[3] == 7U;
    }

    bool scan_activity_may_exist_ = false;
    bool synthetic_idle_disable_ = false;
};

} // namespace bridge

#endif
