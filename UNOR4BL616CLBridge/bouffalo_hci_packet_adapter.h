#ifndef UNOR4_BL616CL_BOUFFALO_HCI_PACKET_ADAPTER_H
#define UNOR4_BL616CL_BOUFFALO_HCI_PACKET_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

#include "ble_hci_service.h"

namespace bridge {

/*
 * Bouffalo on-chip HCI callbacks expose message class, source id, and
 * parameters rather than an H:4 byte stream. This adapter reconstructs the
 * H:4 packets expected by the RA4M1-facing BLE transport.
 */
enum class BouffaloHciPacketType : uint8_t {
    Command = 0U,
    AclData = 1U,
    CommandComplete = 2U,
    CommandStatus = 3U,
    LeEvent = 4U,
    Event = 5U,
    DebugEvent = 7U,
};

class BouffaloHciPacketAdapter {
public:
    void set_receiver(BleHciController::ReceivePacket receiver, void *receiver_context);
    void clear_receiver();

    bool receive_callback(
        BouffaloHciPacketType packet_type,
        uint16_t source_id,
        const uint8_t *parameters,
        size_t parameter_size,
        uint8_t *packet,
        size_t packet_capacity);
    bool receive_acl(
        const uint8_t *acl_packet,
        size_t acl_packet_size,
        uint8_t *packet,
        size_t packet_capacity);

private:
    bool emit(const uint8_t *packet, size_t packet_size) const;

    BleHciController::ReceivePacket receiver_ = nullptr;
    void *receiver_context_ = nullptr;
};

} // namespace bridge

#endif
