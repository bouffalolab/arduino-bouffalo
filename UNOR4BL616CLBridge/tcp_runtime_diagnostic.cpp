#include "tcp_runtime_diagnostic.h"

#if defined(ARDUINO)
extern "C" {
#include <lwip/api.h>
#include <lwip/priv/tcp_priv.h>
#include <lwip/priv/sockets_priv.h>
#include <lwip/sockets.h>
#include <lwip/tcpip.h>
}
#endif

namespace bridge {

namespace {

TcpRuntimeDiagnostic diagnostic;

uint32_t listener_stage_flag(TcpListenerDiagnosticStage stage)
{
    return 1U << static_cast<uint8_t>(stage);
}

} // namespace

void tcp_runtime_diagnostic_begin_listener(uint16_t port)
{
    const uint32_t generation = diagnostic.generation + 1U;
    diagnostic = {};
    diagnostic.generation = generation;
    diagnostic.requested_port = port;
    diagnostic.listener_handle = -1;
    ++diagnostic.listener_create_count;
}

void tcp_runtime_diagnostic_note_listener_stage(
    TcpListenerDiagnosticStage stage,
    int result,
    int error)
{
    if (result == 0) {
        diagnostic.listener_flags |= listener_stage_flag(stage);
    } else {
        diagnostic.last_listener_errno = error;
    }
}

void tcp_runtime_diagnostic_note_listener_ready(int handle)
{
    diagnostic.listener_handle = handle;
    diagnostic.listener_flags |= 1U << 5U;
}

void tcp_runtime_diagnostic_note_listener_address(
    uint32_t configured_address,
    uint32_t observed_address,
    uint16_t observed_port,
    int observed_error)
{
    diagnostic.configured_local_address = configured_address;
    diagnostic.observed_local_address = observed_address;
    diagnostic.observed_local_port = observed_port;
    diagnostic.observed_local_errno = observed_error;
}

void tcp_runtime_diagnostic_note_accept(int result, int error)
{
    ++diagnostic.accept_attempt_count;
    if (result >= 0) {
        ++diagnostic.accept_success_count;
        diagnostic.accepted_handle = result;
        return;
    }
    if (error == 11 || error == 35) {
        ++diagnostic.accept_would_block_count;
        return;
    }
    ++diagnostic.accept_failure_count;
    diagnostic.last_accept_errno = error;
}

void tcp_runtime_diagnostic_note_accepted_socket(
    int handle,
    int peer_result,
    int peer_error,
    int socket_result,
    int socket_error,
    int so_error)
{
    diagnostic.accepted_handle = handle;
    diagnostic.accepted_peer_errno = peer_result == 0 ? 0 : peer_error;
    diagnostic.accepted_socket_errno = socket_result == 0 ? 0 : socket_error;
    diagnostic.accepted_socket_error = so_error;
    diagnostic.accepted_flags = 1U;
    if (peer_result == 0) {
        diagnostic.accepted_flags |= 1U << 1U;
    }
    if (socket_result == 0) {
        diagnostic.accepted_flags |= 1U << 2U;
    }
    if (socket_result == 0 && so_error == 0) {
        diagnostic.accepted_flags |= 1U << 3U;
    }
}

void tcp_runtime_diagnostic_note_available(int result, int error)
{
    ++diagnostic.available_probe_count;
    if (result > 0) {
        ++diagnostic.available_ready_count;
        return;
    }
    if (result == 0) {
        ++diagnostic.available_eof_count;
        return;
    }
    if (error == 11 || error == 35) {
        ++diagnostic.available_would_block_count;
        return;
    }
    ++diagnostic.available_failure_count;
    diagnostic.last_available_errno = error;
}

void tcp_runtime_diagnostic_note_close_request(int handle, TcpCloseOrigin origin)
{
    ++diagnostic.close_request_count;
    if (handle == diagnostic.listener_handle) {
        ++diagnostic.listener_close_request_count;
    }
    diagnostic.last_close_handle = handle;
    diagnostic.last_close_origin = origin;
    diagnostic.last_close_generation = diagnostic.generation;
}

void tcp_runtime_diagnostic_note_close_result(int handle, int result, int error)
{
    diagnostic.last_close_handle = handle;
    diagnostic.last_close_result = result;
    diagnostic.last_close_errno = result == 0 ? 0 : error;
    diagnostic.last_close_generation = diagnostic.generation;
}

TcpRuntimeDiagnostic tcp_runtime_diagnostic_snapshot()
{
    return diagnostic;
}

TcpRuntimeDiagnostic tcp_runtime_diagnostic_lwip_snapshot()
{
#if defined(ARDUINO)
    LOCK_TCPIP_CORE();

    diagnostic.listen_pcb_count = 0U;
    diagnostic.matching_listen_pcb_count = 0U;
    diagnostic.listener_socket_flags = 0U;
    diagnostic.listener_netconn_state = 0U;
    diagnostic.listener_netconn_type = 0U;
    diagnostic.listener_pcb_state = 0U;
    diagnostic.listener_pcb_local_port = 0U;
    diagnostic.listener_pcb_local_address = 0U;
    diagnostic.listen_pcbs[0] = {};
    diagnostic.listen_pcbs[1] = {};
    diagnostic.listen_pcbs[2] = {};

    uint8_t stored_count = 0U;
    for (struct tcp_pcb_listen *pcb = tcp_listen_pcbs.listen_pcbs;
         pcb != nullptr;
         pcb = pcb->next) {
        ++diagnostic.listen_pcb_count;
        if (pcb->local_port != diagnostic.requested_port) {
            continue;
        }

        ++diagnostic.matching_listen_pcb_count;
        if (stored_count >= sizeof(diagnostic.listen_pcbs) /
                                sizeof(diagnostic.listen_pcbs[0])) {
            continue;
        }

        TcpRuntimeDiagnostic::ListenPcb &entry =
            diagnostic.listen_pcbs[stored_count++];
        entry.local_address = ip_2_ip4(&pcb->local_ip)->addr;
        entry.local_port = pcb->local_port;
        entry.netif_index = pcb->netif_idx;
        entry.state = static_cast<uint8_t>(pcb->state);
    }

    if (diagnostic.listener_handle >= 0) {
        struct lwip_sock *socket =
            lwip_socket_dbg_get_socket(diagnostic.listener_handle);
        if (socket != nullptr) {
            diagnostic.listener_socket_flags |= 1U << 0U;
        }
        if (socket != nullptr && socket->conn != nullptr) {
            diagnostic.listener_socket_flags |= 1U << 1U;
        }
        if (socket != nullptr && socket->conn != nullptr &&
            socket->conn->pcb.tcp != nullptr) {
            diagnostic.listener_socket_flags |= 1U << 2U;
        }
        if (socket != nullptr && socket->conn != nullptr &&
            socket->conn->pcb.tcp != nullptr &&
            socket->conn->pcb.tcp->state == LISTEN) {
            diagnostic.listener_socket_flags |= 1U << 3U;
        }
        if (socket != nullptr) {
            struct netconn *connection = socket->conn;
            if (connection != nullptr) {
                diagnostic.listener_netconn_state =
                    static_cast<uint8_t>(connection->state);
                diagnostic.listener_netconn_type =
                    static_cast<uint8_t>(connection->type);
                if (connection->pcb.tcp != nullptr) {
                    const struct tcp_pcb *pcb = connection->pcb.tcp;
                    diagnostic.listener_pcb_state =
                        static_cast<uint8_t>(pcb->state);
                    diagnostic.listener_pcb_local_port = pcb->local_port;
                    diagnostic.listener_pcb_local_address =
                        ip_2_ip4(&pcb->local_ip)->addr;
                }
            }
        }
    }

    const TcpRuntimeDiagnostic snapshot = diagnostic;
    UNLOCK_TCPIP_CORE();
    return snapshot;
#else
    return diagnostic;
#endif
}

} // namespace bridge
