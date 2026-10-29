#include "ap_packet_diagnostic.h"

namespace bridge {

namespace {

ApPacketDiagnostic diagnostic;
constexpr uint8_t kTcpSyn = 0x02U;
constexpr uint8_t kTcpAck = 0x10U;
constexpr uint8_t kTcpReset = 0x04U;

} // namespace

void ap_packet_diagnostic_note_packet(
    bool is_sta,
    bool ipv4,
    bool tcp,
    bool listener_port,
    uint16_t source_port,
    uint16_t destination_port,
    uint8_t tcp_flags,
    uint32_t source_address,
    uint32_t destination_address,
    bool malformed)
{
    ++diagnostic.input_packet_count;
    if (is_sta) {
        ++diagnostic.sta_packet_count;
    } else {
        ++diagnostic.ap_packet_count;
    }
    if (!is_sta && ipv4) {
        ++diagnostic.ap_ipv4_packet_count;
    }
    if (!is_sta && tcp) {
        ++diagnostic.ap_tcp_packet_count;
    }
    if (!is_sta && listener_port) {
        ++diagnostic.ap_tcp_listener_port_count;
        diagnostic.last_listener_source_address = source_address;
        diagnostic.last_listener_destination_address = destination_address;
    }
    if (malformed) {
        ++diagnostic.malformed_packet_count;
    }

    ++diagnostic.last_packet_sequence;
    diagnostic.last_packet_is_sta = is_sta ? 1U : 0U;
    diagnostic.last_packet_ipv4 = ipv4 ? 1U : 0U;
    diagnostic.last_packet_tcp = tcp ? 1U : 0U;
    diagnostic.last_tcp_source_port = source_port;
    diagnostic.last_tcp_destination_port = destination_port;
    diagnostic.last_tcp_flags = tcp_flags;
}

void ap_packet_diagnostic_note_output_packet(
    bool is_sta,
    bool tcp,
    bool listener_port,
    uint16_t source_port,
    uint16_t destination_port,
    uint8_t tcp_flags)
{
    if (!is_sta && tcp && listener_port) {
        ++diagnostic.ap_tcp_listener_output_count;
        if ((tcp_flags & (kTcpSyn | kTcpAck)) == (kTcpSyn | kTcpAck)) {
            ++diagnostic.ap_tcp_listener_syn_ack_output_count;
        }
        if ((tcp_flags & kTcpReset) != 0U) {
            ++diagnostic.ap_tcp_listener_reset_output_count;
        }
    }

    diagnostic.last_output_packet_is_sta = is_sta ? 1U : 0U;
    diagnostic.last_output_tcp_source_port = source_port;
    diagnostic.last_output_tcp_destination_port = destination_port;
    diagnostic.last_output_tcp_flags = tcp_flags;
}

void ap_packet_diagnostic_note_udp_packet(
    bool is_sta,
    bool listener_port,
    uint16_t source_port,
    uint16_t destination_port,
    uint32_t source_address,
    uint32_t destination_address)
{
    if (!is_sta) {
        ++diagnostic.ap_udp_packet_count;
        if (listener_port) {
            ++diagnostic.ap_udp_listener_port_count;
        }
    }

    diagnostic.last_udp_packet_is_sta = is_sta ? 1U : 0U;
    if (!is_sta && source_port == 68U && destination_port == 67U)
        ++diagnostic.ap_dhcp_client_input_count;
    if (is_sta && source_port == 67U && destination_port == 68U)
        ++diagnostic.sta_dhcp_server_input_count;
    diagnostic.last_udp_source_port = source_port;
    diagnostic.last_udp_destination_port = destination_port;
    diagnostic.last_udp_source_address = source_address;
    diagnostic.last_udp_destination_address = destination_address;
}

void ap_packet_diagnostic_note_udp_output_packet(
    bool is_sta,
    bool listener_port,
    uint16_t source_port,
    uint16_t destination_port)
{
    if (!is_sta && listener_port) {
        ++diagnostic.ap_udp_listener_output_count;
    }

    diagnostic.last_output_udp_packet_is_sta = is_sta ? 1U : 0U;
    // The pinned SDK currently hardcodes the output hook's is_sta=true.
    // Count server-port output without inferring its VIF from that flag.
    if (source_port == 67U && destination_port == 68U)
        ++diagnostic.dhcp_server_output_count;
    diagnostic.last_output_udp_source_port = source_port;
    diagnostic.last_output_udp_destination_port = destination_port;
}

ApPacketDiagnostic ap_packet_diagnostic_snapshot()
{
    return diagnostic;
}

void ap_packet_diagnostic_note_dhcp_output(uint8_t message_type)
{
    if (message_type == 2U) ++diagnostic.dhcp_offer_output_count;
    if (message_type == 5U) ++diagnostic.dhcp_ack_output_count;
    if (message_type == 6U) ++diagnostic.dhcp_nak_output_count;
}

} // namespace bridge
