#ifndef UNOR4_BL616CL_TCP_RUNTIME_DIAGNOSTIC_H
#define UNOR4_BL616CL_TCP_RUNTIME_DIAGNOSTIC_H

#include <stdint.h>

namespace bridge {

enum class TcpListenerDiagnosticStage : uint8_t {
    Socket = 0U,
    ReuseAddress = 1U,
    Nonblocking = 2U,
    Bind = 3U,
    Listen = 4U,
    BindToDevice = 6U,
};

enum class TcpCloseOrigin : uint8_t {
    None = 0U,
    TcpClientSlot = 1U,
    TcpServerClientSlot = 2U,
    TcpServerEnd = 3U,
    SoftResetWifi = 4U,
    ListenerSetupFailure = 5U,
};

struct TcpRuntimeDiagnostic {
    struct ListenPcb {
        uint32_t local_address = 0U;
        uint16_t local_port = 0U;
        uint8_t netif_index = 0U;
        uint8_t state = 0U;
    };

    uint32_t generation = 0U;
    uint16_t requested_port = 0U;
    int listener_handle = -1;
    uint32_t listener_flags = 0U;
    int last_listener_errno = 0;
    uint32_t listener_create_count = 0U;
    uint32_t accept_attempt_count = 0U;
    uint32_t accept_success_count = 0U;
    uint32_t accept_would_block_count = 0U;
    uint32_t accept_failure_count = 0U;
    int last_accept_errno = 0;
    int accepted_handle = -1;
    int accepted_peer_errno = 0;
    int accepted_socket_errno = 0;
    int accepted_socket_error = 0;
    uint32_t accepted_flags = 0U;
    uint32_t available_probe_count = 0U;
    uint32_t available_ready_count = 0U;
    uint32_t available_eof_count = 0U;
    uint32_t available_would_block_count = 0U;
    uint32_t available_failure_count = 0U;
    int last_available_errno = 0;
    uint32_t configured_local_address = 0U;
    uint32_t observed_local_address = 0U;
    uint16_t observed_local_port = 0U;
    int observed_local_errno = 0;
    uint8_t listen_pcb_count = 0U;
    uint8_t matching_listen_pcb_count = 0U;
    uint8_t listener_socket_flags = 0U;
    uint8_t listener_netconn_state = 0U;
    uint8_t listener_netconn_type = 0U;
    uint8_t listener_pcb_state = 0U;
    uint16_t listener_pcb_local_port = 0U;
    uint32_t listener_pcb_local_address = 0U;
    uint32_t close_request_count = 0U;
    uint32_t listener_close_request_count = 0U;
    int last_close_handle = -1;
    TcpCloseOrigin last_close_origin = TcpCloseOrigin::None;
    int last_close_result = 0;
    int last_close_errno = 0;
    uint32_t last_close_generation = 0U;
    ListenPcb listen_pcbs[3] = {};
};

void tcp_runtime_diagnostic_begin_listener(uint16_t port);
void tcp_runtime_diagnostic_note_listener_stage(
    TcpListenerDiagnosticStage stage,
    int result,
    int error);
void tcp_runtime_diagnostic_note_listener_ready(int handle);
void tcp_runtime_diagnostic_note_listener_address(
    uint32_t configured_address,
    uint32_t observed_address,
    uint16_t observed_port,
    int observed_error);
void tcp_runtime_diagnostic_note_accept(int result, int error);
void tcp_runtime_diagnostic_note_accepted_socket(
    int handle,
    int peer_result,
    int peer_error,
    int socket_result,
    int socket_error,
    int so_error);
void tcp_runtime_diagnostic_note_available(int result, int error);
void tcp_runtime_diagnostic_note_close_request(int handle, TcpCloseOrigin origin);
void tcp_runtime_diagnostic_note_close_result(int handle, int result, int error);
TcpRuntimeDiagnostic tcp_runtime_diagnostic_snapshot();
TcpRuntimeDiagnostic tcp_runtime_diagnostic_lwip_snapshot();

} // namespace bridge

#endif
