#ifndef UNOR4_BL616CL_AP_PACKET_DIAGNOSTIC_H
#define UNOR4_BL616CL_AP_PACKET_DIAGNOSTIC_H

#include <stdint.h>

namespace bridge {

struct ApPacketDiagnostic {
    uint32_t input_packet_count = 0U;
    uint32_t sta_packet_count = 0U;
    uint32_t ap_packet_count = 0U;
    uint32_t ap_ipv4_packet_count = 0U;
    uint32_t ap_tcp_packet_count = 0U;
    uint32_t ap_tcp_listener_port_count = 0U;
    uint32_t ap_tcp_listener_output_count = 0U;
    uint32_t ap_tcp_listener_syn_ack_output_count = 0U;
    uint32_t ap_tcp_listener_reset_output_count = 0U;
    uint32_t ap_udp_packet_count = 0U;
    uint32_t ap_udp_listener_port_count = 0U;
    uint32_t ap_udp_listener_output_count = 0U;
    uint32_t ap_dhcp_client_input_count = 0U;
    uint32_t sta_dhcp_server_input_count = 0U;
    uint32_t dhcp_server_output_count = 0U;
    uint32_t dhcp_offer_output_count = 0U;
    uint32_t dhcp_ack_output_count = 0U;
    uint32_t dhcp_nak_output_count = 0U;
    uint32_t malformed_packet_count = 0U;
    uint32_t last_packet_sequence = 0U;
    uint16_t last_tcp_source_port = 0U;
    uint16_t last_tcp_destination_port = 0U;
    uint32_t last_listener_source_address = 0U;
    uint32_t last_listener_destination_address = 0U;
    uint8_t last_tcp_flags = 0U;
    uint8_t last_packet_is_sta = 0U;
    uint8_t last_packet_ipv4 = 0U;
    uint8_t last_packet_tcp = 0U;
    uint16_t last_output_tcp_source_port = 0U;
    uint16_t last_output_tcp_destination_port = 0U;
    uint16_t last_udp_source_port = 0U;
    uint16_t last_udp_destination_port = 0U;
    uint16_t last_output_udp_source_port = 0U;
    uint16_t last_output_udp_destination_port = 0U;
    uint32_t last_udp_source_address = 0U;
    uint32_t last_udp_destination_address = 0U;
    uint8_t last_output_tcp_flags = 0U;
    uint8_t last_output_packet_is_sta = 0U;
    uint8_t last_udp_packet_is_sta = 0U;
    uint8_t last_output_udp_packet_is_sta = 0U;
};

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
    bool malformed);
void ap_packet_diagnostic_note_output_packet(
    bool is_sta,
    bool tcp,
    bool listener_port,
    uint16_t source_port,
    uint16_t destination_port,
    uint8_t tcp_flags);
void ap_packet_diagnostic_note_udp_packet(
    bool is_sta,
    bool listener_port,
    uint16_t source_port,
    uint16_t destination_port,
    uint32_t source_address,
    uint32_t destination_address);
void ap_packet_diagnostic_note_udp_output_packet(
    bool is_sta,
    bool listener_port,
    uint16_t source_port,
    uint16_t destination_port);
void ap_packet_diagnostic_note_dhcp_output(uint8_t message_type);
ApPacketDiagnostic ap_packet_diagnostic_snapshot();

} // namespace bridge

#endif
