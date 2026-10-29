#ifndef UNOR4_BL616CL_UDP_SOCKET_DIAGNOSTIC_H
#define UNOR4_BL616CL_UDP_SOCKET_DIAGNOSTIC_H

#include <stdint.h>

namespace bridge {

/*
 * Read-only snapshot for diagnosing a live WiFiUDP socket in AP+STA mode.
 * It is exposed only through the bridge's diagnostic HID feature page.
 */
struct UdpSocketDiagnostic {
    uint8_t pcb_count = 0U;
    uint8_t matching_port_pcb_count = 0U;
    uint8_t socket_pcb_found = 0U;
    uint8_t socket_pcb_netif_index = 0U;
    uint8_t socket_pcb_flags = 0U;
    uint8_t socket_netconn_state = 0U;
    uint8_t socket_netconn_type = 0U;
    int8_t socket_error = 0;
    uint16_t socket_local_port = 0U;
    uint16_t socket_remote_port = 0U;
    uint32_t socket_local_address = 0U;
    uint16_t udp_recv_delta = 0U;
    uint16_t udp_drop_delta = 0U;
    uint16_t udp_checksum_error_delta = 0U;
    uint16_t udp_length_error_delta = 0U;
    uint16_t udp_protocol_error_delta = 0U;
    uint16_t udp_route_error_delta = 0U;
    uint16_t udp_xmit_delta = 0U;
    uint16_t receive_call_count = 0U;
    uint16_t receive_would_block_count = 0U;
    uint16_t receive_success_count = 0U;
    uint16_t receive_failure_count = 0U;
    uint16_t receive_restore_failure_count = 0U;
    uint16_t receive_byte_count = 0U;
    int16_t last_receive_result = 0;
    int8_t last_receive_errno = 0;
    uint16_t parse_attempt_count = 0U;
    uint8_t parse_reject_mask = 0U;
    uint8_t last_parse_mode = 0U;
    uint8_t last_parse_argument_count = 0U;
    uint8_t last_parse_slot = 0U;
    uint8_t last_parse_slot_open = 0U;
    uint8_t last_parse_backend_present = 0U;
};

UdpSocketDiagnostic udp_socket_diagnostic_snapshot();
void udp_socket_diagnostic_note_parse(
    uint8_t mode,
    uint8_t argument_count,
    uint8_t slot,
    bool slot_valid,
    bool slot_open,
    bool backend_present);

} // namespace bridge

#endif
