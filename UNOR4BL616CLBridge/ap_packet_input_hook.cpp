#include "ipv6_tcp_diagnostic.h"
#include "ap_packet_input_hook.h"

#include "ap_packet_diagnostic.h"
#include "sta_tcp_diagnostic.h"
#include "sta_rx_ownership.h"
#include "dhcp_ack_packet.h"

extern "C" {
#include <lwip/pbuf.h>
}

namespace bridge {

namespace {

constexpr uint16_t kEtherTypeIpv4 = 0x0800U;
constexpr uint8_t kIpv4ProtocolTcp = 6U;
constexpr uint8_t kIpv4ProtocolUdp = 17U;
constexpr uint16_t kListenerPort = 4243U;
constexpr uint16_t kUdpListenerPort = 4244U;
constexpr uint16_t kEthernetHeaderLength = 14U;
constexpr uint8_t kIpv4MinimumHeaderLength = 20U;
constexpr uint8_t kTcpPortHeaderLength = 14U;
constexpr uint8_t kUdpPortHeaderLength = 8U;
constexpr size_t kIpv4SourceAddressOffset = 12U;
constexpr size_t kIpv4DestinationAddressOffset = 16U;

typedef void *(*PacketInputHook)(bool is_sta, void *packet, void *context);

extern "C" void bl_pkt_eth_input_hook_register(
    PacketInputHook callback,
    void *context);
extern "C" void bl_pkt_eth_output_hook_register(
    PacketInputHook callback,
    void *context);

uint16_t read_be_u16(const uint8_t *data)
{
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(data[0]) << 8U) |
        static_cast<uint16_t>(data[1]));
}

uint32_t read_be_u32(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 24U) |
        (static_cast<uint32_t>(data[1]) << 16U) |
        (static_cast<uint32_t>(data[2]) << 8U) |
        static_cast<uint32_t>(data[3]);
}

void *observe_packet(bool is_sta, void *packet, void *)
{
    ipv6_tcp_diagnostic_observe(false, packet);
#if BL616CL_STA_TCP_DIAGNOSTIC
    sta_tcp_observe(is_sta, false, packet);
#endif
    struct pbuf *pbuf = static_cast<struct pbuf *>(packet);
    if (pbuf == nullptr) {
        ap_packet_diagnostic_note_packet(
            is_sta, false, false, false, 0U, 0U, 0U, 0U, 0U, true);
        return packet;
    }

    uint8_t ethernet[kEthernetHeaderLength] = {};
    if (pbuf_copy_partial(pbuf, ethernet, sizeof(ethernet), 0U) !=
        sizeof(ethernet)) {
        ap_packet_diagnostic_note_packet(
            is_sta, false, false, false, 0U, 0U, 0U, 0U, 0U, true);
        return packet;
    }
    if (read_be_u16(&ethernet[12]) != kEtherTypeIpv4) {
        ap_packet_diagnostic_note_packet(
            is_sta, false, false, false, 0U, 0U, 0U, 0U, 0U, false);
        return packet;
    }

    uint8_t ip_header[kIpv4MinimumHeaderLength] = {};
    if (pbuf_copy_partial(
            pbuf,
            ip_header,
            sizeof(ip_header),
            kEthernetHeaderLength) != sizeof(ip_header)) {
        ap_packet_diagnostic_note_packet(
            is_sta, true, false, false, 0U, 0U, 0U, 0U, 0U, true);
        return packet;
    }

    const uint8_t version = static_cast<uint8_t>(ip_header[0] >> 4U);
    const uint8_t header_length = static_cast<uint8_t>((ip_header[0] & 0x0FU) * 4U);
    if (version != 4U || header_length < kIpv4MinimumHeaderLength) {
        ap_packet_diagnostic_note_packet(
            is_sta, true, false, false, 0U, 0U, 0U, 0U, 0U, true);
        return packet;
    }
    const uint32_t source_address = read_be_u32(
        &ip_header[kIpv4SourceAddressOffset]);
    const uint32_t destination_address = read_be_u32(
        &ip_header[kIpv4DestinationAddressOffset]);
    if (ip_header[9] != kIpv4ProtocolTcp &&
        ip_header[9] != kIpv4ProtocolUdp) {
        ap_packet_diagnostic_note_packet(
            is_sta, true, false, false, 0U, 0U, 0U, 0U, 0U, false);
        return packet;
    }

    uint8_t transport_header[kTcpPortHeaderLength] = {};
    if (pbuf_copy_partial(
            pbuf,
            transport_header,
            ip_header[9] == kIpv4ProtocolTcp
                ? sizeof(transport_header)
                : kUdpPortHeaderLength,
            static_cast<u16_t>(kEthernetHeaderLength + header_length)) !=
        (ip_header[9] == kIpv4ProtocolTcp
            ? sizeof(transport_header)
            : kUdpPortHeaderLength)) {
        ap_packet_diagnostic_note_packet(
            is_sta, true, ip_header[9] == kIpv4ProtocolTcp,
            false, 0U, 0U, 0U, 0U, 0U, true);
        return packet;
    }

    const uint16_t source_port = read_be_u16(&transport_header[0]);
    const uint16_t destination_port = read_be_u16(&transport_header[2]);
    if (ip_header[9] == kIpv4ProtocolUdp) {
        ap_packet_diagnostic_note_udp_packet(
            is_sta,
            destination_port == kUdpListenerPort,
            source_port,
            destination_port,
            source_address,
            destination_address);
        return packet;
    }
    ap_packet_diagnostic_note_packet(
        is_sta,
        true,
        true,
        destination_port == kListenerPort,
        source_port,
        destination_port,
        transport_header[13],
        source_address,
        destination_address,
        false);
    return packet;
}

void *observe_output_packet(bool is_sta, void *packet, void *)
{
    ipv6_tcp_diagnostic_observe(true, packet);
#if BL616CL_STA_TCP_DIAGNOSTIC
    sta_tcp_observe(is_sta, true, packet);
#endif
    struct pbuf *pbuf = static_cast<struct pbuf *>(packet);
    if (pbuf == nullptr) {
        return packet;
    }

    uint8_t ethernet[kEthernetHeaderLength] = {};
    if (pbuf_copy_partial(pbuf, ethernet, sizeof(ethernet), 0U) !=
            sizeof(ethernet) ||
        read_be_u16(&ethernet[12]) != kEtherTypeIpv4) {
        return packet;
    }

    uint8_t ip_header[kIpv4MinimumHeaderLength] = {};
    if (pbuf_copy_partial(
            pbuf,
            ip_header,
            sizeof(ip_header),
            kEthernetHeaderLength) != sizeof(ip_header)) {
        return packet;
    }

    const uint8_t version = static_cast<uint8_t>(ip_header[0] >> 4U);
    const uint8_t header_length = static_cast<uint8_t>((ip_header[0] & 0x0FU) * 4U);
    if (version != 4U || header_length < kIpv4MinimumHeaderLength ||
        (ip_header[9] != kIpv4ProtocolTcp &&
         ip_header[9] != kIpv4ProtocolUdp)) {
        return packet;
    }

    uint8_t transport_header[kTcpPortHeaderLength] = {};
    if (pbuf_copy_partial(
            pbuf,
            transport_header,
            ip_header[9] == kIpv4ProtocolTcp
                ? sizeof(transport_header)
                : kUdpPortHeaderLength,
            static_cast<u16_t>(kEthernetHeaderLength + header_length)) !=
        (ip_header[9] == kIpv4ProtocolTcp
            ? sizeof(transport_header)
            : kUdpPortHeaderLength)) {
        return packet;
    }

    const uint16_t source_port = read_be_u16(&transport_header[0]);
    const uint16_t destination_port = read_be_u16(&transport_header[2]);
    if (ip_header[9] == kIpv4ProtocolUdp) {
        ap_packet_diagnostic_note_udp_output_packet(
            is_sta,
            source_port == kUdpListenerPort,
            source_port,
            destination_port);
        if (source_port == 67U && destination_port == 68U) {
            uint8_t prefix[14U + 60U + 8U + 240U + 64U] = {};
            const size_t copied = pbuf_copy_partial(pbuf, prefix, sizeof(prefix), 0U);
            ap_packet_diagnostic_note_dhcp_output(dhcp_server_message_type(prefix, copied));
        }
        return packet;
    }
    ap_packet_diagnostic_note_output_packet(
        is_sta,
        true,
        source_port == kListenerPort,
        source_port,
        destination_port,
        transport_header[13]);
    return packet;
}

} // namespace

namespace {
void *receive_packet(bool is_sta, void *packet, void *context) {
    return observe_packet(is_sta, sta_rx_detach(is_sta, packet), context);
}
}

void ap_packet_input_hook_begin()
{
    /*
     * The SDK calls this hook immediately before lwIP input. Header observers
     * are passive; the optional STA TCP ownership experiment precedes them.
     */
    bl_pkt_eth_input_hook_register(receive_packet, nullptr);
    bl_pkt_eth_output_hook_register(observe_output_packet, nullptr);
}

} // namespace bridge
