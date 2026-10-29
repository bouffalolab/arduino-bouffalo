#include "bouffalo_hci_packet_adapter.h"

#include <string.h>

namespace bridge {

namespace {

constexpr uint8_t kH4AclData = 0x02U;
constexpr uint8_t kH4Event = 0x04U;

constexpr uint8_t kEventCommandComplete = 0x0EU;
constexpr uint8_t kEventCommandStatus = 0x0FU;
constexpr uint8_t kEventLeMeta = 0x3EU;
constexpr uint8_t kEventVendor = 0xFFU;
constexpr uint8_t kHostCommandCredits = 0x01U;

bool has_capacity(size_t packet_capacity, size_t packet_size)
{
    return packet_size <= packet_capacity && packet_size <= BleHciService::kMaxPacketBytes;
}

void write_le16(uint8_t *destination, uint16_t value)
{
    destination[0] = static_cast<uint8_t>(value & 0xFFU);
    destination[1] = static_cast<uint8_t>(value >> 8U);
}

} // namespace

void BouffaloHciPacketAdapter::set_receiver(
    BleHciController::ReceivePacket receiver,
    void *receiver_context)
{
    receiver_ = receiver;
    receiver_context_ = receiver_context;
}

void BouffaloHciPacketAdapter::clear_receiver()
{
    receiver_ = nullptr;
    receiver_context_ = nullptr;
}

bool BouffaloHciPacketAdapter::receive_callback(
    BouffaloHciPacketType packet_type,
    uint16_t source_id,
    const uint8_t *parameters,
    size_t parameter_size,
    uint8_t *packet,
    size_t packet_capacity)
{
    if (packet == nullptr || (parameters == nullptr && parameter_size != 0U)) {
        return false;
    }

    size_t packet_size = 0U;
    switch (packet_type) {
    case BouffaloHciPacketType::CommandComplete:
        if (parameter_size > 252U || !has_capacity(packet_capacity, parameter_size + 6U)) {
            return false;
        }
        packet[0] = kH4Event;
        packet[1] = kEventCommandComplete;
        packet[2] = static_cast<uint8_t>(parameter_size + 3U);
        packet[3] = kHostCommandCredits;
        write_le16(packet + 4U, source_id);
        if (parameter_size != 0U) {
            memcpy(packet + 6U, parameters, parameter_size);
        }
        packet_size = parameter_size + 6U;
        break;

    case BouffaloHciPacketType::CommandStatus:
        if (parameter_size != 1U || !has_capacity(packet_capacity, 7U)) {
            return false;
        }
        packet[0] = kH4Event;
        packet[1] = kEventCommandStatus;
        packet[2] = 4U;
        packet[3] = parameters[0];
        packet[4] = kHostCommandCredits;
        write_le16(packet + 5U, source_id);
        packet_size = 7U;
        break;

    case BouffaloHciPacketType::LeEvent:
        if (parameter_size > 255U || !has_capacity(packet_capacity, parameter_size + 3U)) {
            return false;
        }
        packet[0] = kH4Event;
        packet[1] = kEventLeMeta;
        packet[2] = static_cast<uint8_t>(parameter_size);
        if (parameter_size != 0U) {
            memcpy(packet + 3U, parameters, parameter_size);
        }
        packet_size = parameter_size + 3U;
        break;

    case BouffaloHciPacketType::Event:
        if (source_id > 0xFFU || parameter_size > 255U ||
            !has_capacity(packet_capacity, parameter_size + 3U)) {
            return false;
        }
        packet[0] = kH4Event;
        packet[1] = static_cast<uint8_t>(source_id);
        packet[2] = static_cast<uint8_t>(parameter_size);
        if (parameter_size != 0U) {
            memcpy(packet + 3U, parameters, parameter_size);
        }
        packet_size = parameter_size + 3U;
        break;

    case BouffaloHciPacketType::DebugEvent:
        if (parameter_size > 255U || !has_capacity(packet_capacity, parameter_size + 3U)) {
            return false;
        }
        packet[0] = kH4Event;
        packet[1] = kEventVendor;
        packet[2] = static_cast<uint8_t>(parameter_size);
        if (parameter_size != 0U) {
            memcpy(packet + 3U, parameters, parameter_size);
        }
        packet_size = parameter_size + 3U;
        break;

    default:
        return false;
    }

    return emit(packet, packet_size);
}

bool BouffaloHciPacketAdapter::receive_acl(
    const uint8_t *acl_packet,
    size_t acl_packet_size,
    uint8_t *packet,
    size_t packet_capacity)
{
    if (acl_packet == nullptr || packet == nullptr ||
        !has_capacity(packet_capacity, acl_packet_size + 1U)) {
        return false;
    }

    packet[0] = kH4AclData;
    memmove(packet + 1U, acl_packet, acl_packet_size);
    return emit(packet, acl_packet_size + 1U);
}

bool BouffaloHciPacketAdapter::emit(const uint8_t *packet, size_t packet_size) const
{
    if (receiver_ == nullptr || packet == nullptr || packet_size == 0U) {
        return false;
    }

    receiver_(receiver_context_, packet, packet_size);
    return true;
}

} // namespace bridge
