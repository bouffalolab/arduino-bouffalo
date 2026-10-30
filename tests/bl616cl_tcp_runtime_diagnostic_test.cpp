#include "../UNOR4BL616CLBridge/tcp_runtime_diagnostic.h"
#include "../UNOR4BL616CLBridge/ap_packet_diagnostic.h"

#include <stdio.h>
#include <stdlib.h>

namespace {

void expect(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

} // namespace

int main()
{
    bridge::tcp_runtime_diagnostic_begin_listener(4243U);
    bridge::tcp_runtime_diagnostic_note_listener_stage(
        bridge::TcpListenerDiagnosticStage::Socket,
        0,
        0);
    bridge::tcp_runtime_diagnostic_note_listener_stage(
        bridge::TcpListenerDiagnosticStage::ReuseAddress,
        0,
        0);
    bridge::tcp_runtime_diagnostic_note_listener_stage(
        bridge::TcpListenerDiagnosticStage::Nonblocking,
        0,
        0);
    bridge::tcp_runtime_diagnostic_note_listener_stage(
        bridge::TcpListenerDiagnosticStage::Bind,
        0,
        0);
    bridge::tcp_runtime_diagnostic_note_listener_stage(
        bridge::TcpListenerDiagnosticStage::Listen,
        0,
        0);
    bridge::tcp_runtime_diagnostic_note_listener_stage(
        bridge::TcpListenerDiagnosticStage::BindToDevice,
        0,
        0);
    bridge::tcp_runtime_diagnostic_note_listener_ready(7);
    bridge::tcp_runtime_diagnostic_note_accept(-1, 11);
    bridge::tcp_runtime_diagnostic_note_accept(8, 0);
    bridge::tcp_runtime_diagnostic_note_accept(-1, 104);
    bridge::tcp_runtime_diagnostic_note_accepted_socket(
        8,
        0,
        0,
        0,
        0,
        0);
    bridge::tcp_runtime_diagnostic_note_available(-1, 11);
    bridge::tcp_runtime_diagnostic_note_available(1, 0);
    bridge::tcp_runtime_diagnostic_note_available(0, 0);
    bridge::tcp_runtime_diagnostic_note_available(-1, 104);
    bridge::tcp_runtime_diagnostic_note_close_request(
        7,
        bridge::TcpCloseOrigin::TcpServerEnd);
    bridge::tcp_runtime_diagnostic_note_close_result(7, 0, 0);

    const bridge::TcpRuntimeDiagnostic snapshot =
        bridge::tcp_runtime_diagnostic_snapshot();
    expect(snapshot.generation != 0U, "listener generation");
    expect(snapshot.requested_port == 4243U, "listener port");
    expect(snapshot.listener_handle == 7, "listener handle");
    expect(snapshot.listener_flags == 0x7fU, "listener stages");
    expect(snapshot.accept_attempt_count == 3U, "accept attempts");
    expect(snapshot.accept_success_count == 1U, "accept successes");
    expect(snapshot.accept_would_block_count == 1U, "accept would block");
    expect(snapshot.accept_failure_count == 1U, "accept failures");
    expect(snapshot.last_accept_errno == 104, "accept errno");
    expect(snapshot.accepted_flags == 0x0fU, "accepted socket flags");
    expect(snapshot.accepted_handle == 8, "accepted socket handle");
    expect(snapshot.accepted_peer_errno == 0, "accepted peer errno");
    expect(snapshot.accepted_socket_errno == 0, "accepted getsockopt errno");
    expect(snapshot.accepted_socket_error == 0, "accepted socket error");
    expect(snapshot.available_probe_count == 4U, "available probe count");
    expect(snapshot.available_ready_count == 1U, "available ready count");
    expect(snapshot.available_eof_count == 1U, "available eof count");
    expect(snapshot.available_would_block_count == 1U, "available would block count");
    expect(snapshot.available_failure_count == 1U, "available failure count");
    expect(snapshot.last_available_errno == 104, "available errno");
    expect(snapshot.close_request_count == 1U, "close request count");
    expect(snapshot.listener_close_request_count == 1U, "listener close count");
    expect(snapshot.last_close_handle == 7, "last close handle");
    expect(
        snapshot.last_close_origin ==
            bridge::TcpCloseOrigin::TcpServerEnd,
        "last close origin");
    expect(snapshot.last_close_result == 0, "last close result");
    expect(snapshot.last_close_errno == 0, "last close errno");
    expect(
        snapshot.last_close_generation == snapshot.generation,
        "last close generation");

    bridge::ap_packet_diagnostic_note_packet(
        true, false, false, false, 0U, 0U, 0U, 0U, 0U, false);
    bridge::ap_packet_diagnostic_note_packet(
        false, true, true, true, 49152U, 4243U, 0x02U,
        0xC0A82A64U, 0xC0A82A01U, false);
    bridge::ap_packet_diagnostic_note_packet(
        false, true, false, false, 0U, 0U, 0U, 0U, 0U, true);
    const bridge::ApPacketDiagnostic packet =
        bridge::ap_packet_diagnostic_snapshot();
    expect(packet.input_packet_count == 3U, "packet input count");
    expect(packet.sta_packet_count == 1U, "packet STA count");
    expect(packet.ap_packet_count == 2U, "packet AP count");
    expect(packet.ap_ipv4_packet_count == 2U, "packet AP IPv4 count");
    expect(packet.ap_tcp_packet_count == 1U, "packet AP TCP count");
    expect(packet.ap_tcp_listener_port_count == 1U, "packet listener port count");
    expect(packet.malformed_packet_count == 1U, "packet malformed count");
    expect(packet.last_packet_sequence == 3U, "packet sequence");
    expect(packet.last_packet_is_sta == 0U, "last packet AP");
    expect(packet.last_packet_ipv4 == 1U, "last packet IPv4");
    expect(packet.last_packet_tcp == 0U, "last packet TCP");
    expect(
        packet.last_listener_source_address == 0xC0A82A64U,
        "listener source address");
    expect(
        packet.last_listener_destination_address == 0xC0A82A01U,
        "listener destination address");
    bridge::ap_packet_diagnostic_note_output_packet(
        false, true, true, 4243U, 49152U, 0x12U);
    bridge::ap_packet_diagnostic_note_output_packet(
        false, true, true, 4243U, 49152U, 0x14U);
    const bridge::ApPacketDiagnostic output_packet =
        bridge::ap_packet_diagnostic_snapshot();
    expect(output_packet.ap_tcp_listener_output_count == 2U, "listener output count");
    expect(
        output_packet.ap_tcp_listener_syn_ack_output_count == 1U,
        "listener SYN ACK output count");
    expect(
        output_packet.ap_tcp_listener_reset_output_count == 1U,
        "listener RST output count");
    expect(
        output_packet.last_output_tcp_source_port == 4243U,
        "last output TCP source port");
    expect(
        output_packet.last_output_tcp_destination_port == 49152U,
        "last output TCP destination port");
    expect(output_packet.last_output_tcp_flags == 0x14U, "last output TCP flags");
    bridge::ap_packet_diagnostic_note_udp_packet(
        false, true, 49153U, 4244U, 0xC0A82A64U, 0xC0A82A01U);
    bridge::ap_packet_diagnostic_note_udp_output_packet(
        false, true, 4244U, 49153U);
    const bridge::ApPacketDiagnostic udp_packet =
        bridge::ap_packet_diagnostic_snapshot();
    expect(udp_packet.ap_udp_packet_count == 1U, "AP UDP packet count");
    expect(udp_packet.ap_udp_listener_port_count == 1U, "AP UDP listener count");
    expect(udp_packet.ap_udp_listener_output_count == 1U, "AP UDP output count");
    expect(udp_packet.last_udp_source_port == 49153U, "last UDP source port");
    expect(udp_packet.last_udp_destination_port == 4244U, "last UDP destination port");
    expect(
        udp_packet.last_udp_source_address == 0xC0A82A64U,
        "last UDP source address");
    expect(
        udp_packet.last_udp_destination_address == 0xC0A82A01U,
        "last UDP destination address");
    expect(
        udp_packet.last_output_udp_source_port == 4244U,
        "last output UDP source port");
    expect(
        udp_packet.last_output_udp_destination_port == 49153U,
        "last output UDP destination port");
    bridge::ap_packet_diagnostic_note_udp_packet(false, false, 68U, 67U, 0U, 0U);
    bridge::ap_packet_diagnostic_note_udp_packet(true, false, 68U, 67U, 0U, 0U);
    bridge::ap_packet_diagnostic_note_udp_packet(true, false, 67U, 68U, 0U, 0U);
    bridge::ap_packet_diagnostic_note_udp_packet(false, false, 67U, 68U, 0U, 0U);
    bridge::ap_packet_diagnostic_note_udp_output_packet(true, false, 67U, 68U);
    bridge::ap_packet_diagnostic_note_udp_output_packet(false, false, 67U, 68U);
    bridge::ap_packet_diagnostic_note_udp_output_packet(false, false, 68U, 67U);
    const auto dhcp = bridge::ap_packet_diagnostic_snapshot();
    expect(dhcp.ap_dhcp_client_input_count == 1U, "AP DHCP requests only");
    expect(dhcp.sta_dhcp_server_input_count == 1U, "STA DHCP replies only");
    expect(dhcp.dhcp_server_output_count == 2U, "DHCP output independent of unreliable VIF flag");
    const uint8_t message_types[] = {0U, 1U, 2U, 5U, 6U, 9U};
    for (uint8_t type : message_types)
        bridge::ap_packet_diagnostic_note_dhcp_output(type);
    const auto types = bridge::ap_packet_diagnostic_snapshot();
    expect(types.dhcp_offer_output_count == 1U, "DHCP offer classification");
    expect(types.dhcp_ack_output_count == 1U, "DHCP ack classification");
    expect(types.dhcp_nak_output_count == 1U, "DHCP nak classification");
    return 0;
}
