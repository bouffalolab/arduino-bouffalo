#ifndef UNOR4_BL616CL_TLS_CLIENT_SERVICE_H
#define UNOR4_BL616CL_TLS_CLIENT_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "at_transport.h"
#include "tcp_flow_diagnostic.h"

#ifndef BL616CL_TLS_CACHED_AVAILABLE
#define BL616CL_TLS_CACHED_AVAILABLE 0
#endif

namespace bridge {

constexpr size_t kTlsClientSlotCount = 8U;
constexpr int kTlsClientSocketBase = 2000;
constexpr size_t kTlsClientPrivateKeyDerCapacity = 160U;

using TlsPrivateKeyResolver = bool (*)(
    void *context,
    const char *slot,
    uint8_t *destination,
    size_t capacity,
    size_t *size);

struct TlsHeapDiagnostic {
    uint32_t before_handshake_free_bytes = 0U;
    uint32_t failure_free_bytes = 0U;
    uint32_t after_cleanup_free_bytes = 0U;
    uint32_t connection_timeout_ms = 0U;
    uint32_t handshake_elapsed_ms = 0U;
    uint32_t before_setup_ocram_free_bytes = 0U;
    uint32_t before_setup_wram_free_bytes = 0U;
    uint32_t failure_ocram_free_bytes = 0U;
    uint32_t failure_wram_free_bytes = 0U;
    uint32_t after_cleanup_ocram_free_bytes = 0U;
    uint32_t after_cleanup_wram_free_bytes = 0U;
    uint32_t observed_ocram_free_bytes = 0U;
    uint32_t observed_wram_free_bytes = 0U;
    uint32_t before_setup_wram_max_block_bytes = 0U;
    uint32_t failure_wram_max_block_bytes = 0U;
    uint32_t after_cleanup_wram_max_block_bytes = 0U;
    uint32_t observed_wram_max_block_bytes = 0U;
    uint32_t failure_tcp_pcb_used = 0U;
    uint32_t failure_tcp_seg_used = 0U;
    uint32_t failure_netconn_used = 0U;
    uint32_t observed_tcp_pcb_used = 0U;
    uint32_t observed_tcp_seg_used = 0U;
    uint32_t observed_netconn_used = 0U;
    uint32_t tcp_seg_allocation_errors = 0U;
    uint32_t failure_lwip_heap_used = 0U;
    uint32_t observed_lwip_heap_used = 0U;
    uint32_t lwip_heap_allocation_errors = 0U;
    // Observed under the lwIP core lock; states CLOSED through TIME_WAIT.
    uint32_t tcp_state_count[11] = {};
    uint32_t tcp_unacked_pbuf_bytes = 0U;
    uint32_t tcp_unsent_pbuf_bytes = 0U;
    uint32_t tcp_ooseq_pbuf_bytes = 0U;
    uint32_t tcp_max_retransmissions = 0U;
#if BL616CL_TLS_FLOW_DIAGNOSTIC
    TcpFlowDiagnostic flow;
#endif
};

struct TlsEndpointDiagnostic {
    uint8_t resolved_address[4] = {};
    uint8_t peer_address[4] = {};
    uint16_t peer_port = 0U;
    bool resolved_valid = false;
    bool peer_valid = false;
};

struct TlsClientBackend {
    void *context = nullptr;
    bool (*create)(void *context, int *handle) = nullptr;
    bool (*close)(void *context, int handle) = nullptr;
    bool (*connect_hostname)(
        void *context,
        int handle,
        const char *hostname,
        uint16_t port,
        uint32_t timeout_ms,
        const uint8_t *ca_certificate,
        size_t ca_certificate_size,
        const uint8_t *client_certificate,
        size_t client_certificate_size,
        const uint8_t *client_private_key,
        size_t client_private_key_size) = nullptr;
    bool (*connect_ip)(
        void *context,
        int handle,
        const uint8_t address[4],
        uint16_t port,
        uint32_t timeout_ms,
        const uint8_t *ca_certificate,
        size_t ca_certificate_size,
        const uint8_t *client_certificate,
        size_t client_certificate_size,
        const uint8_t *client_private_key,
        size_t client_private_key_size) = nullptr;
    bool (*send)(
        void *context,
        int handle,
        const uint8_t *data,
        size_t size,
        size_t *sent) = nullptr;
    bool (*receive)(
        void *context,
        int handle,
        uint8_t *data,
        size_t capacity,
        size_t *received) = nullptr;
    bool (*available)(void *context, int handle, size_t *bytes) = nullptr;
    bool (*peek)(void *context, int handle, int *value) = nullptr;
    bool (*connected)(void *context, int handle, bool *value) = nullptr;
    bool (*get_state)(
        void *context,
        int handle,
        uint8_t remote_address[4],
        uint16_t *remote_port,
        uint16_t *local_port) = nullptr;
    bool (*flush)(void *context, int handle) = nullptr;
    bool (*get_diagnostic)(
        void *context,
        int handle,
        uint8_t *phase,
        int32_t *error) = nullptr;
    bool (*get_verify_flags)(
        void *context,
        int handle,
        uint32_t *flags) = nullptr;
    bool (*get_heap_diagnostic)(
        void *context,
        int handle,
        TlsHeapDiagnostic *diagnostic) = nullptr;
    bool (*get_endpoint_diagnostic)(
        void *context,
        int handle,
        TlsEndpointDiagnostic *diagnostic) = nullptr;
    bool (*get_local_address)(
        void *context,
        int handle,
        uint8_t local_address[4]) = nullptr;
#if BL616CL_TLS_CACHED_AVAILABLE
    // AT-task only. True means positive plaintext count with no socket I/O,
    // SSL read, state transition, or payload consumption. False uses RF window.
    bool (*cached_available)(void *context, int handle, size_t *bytes) = nullptr;
#endif
};

class TlsClientService {
public:
    using ScanPause = bool (*)(void *context);
    using ScanResume = void (*)(void *context);
    explicit TlsClientService(
        TlsClientBackend backend = {},
        TlsPrivateKeyResolver private_key_resolver = nullptr,
        void *private_key_resolver_context = nullptr);
    ~TlsClientService();

    bool handles_command(const at::Command &command) const;
    at::CommandResult handle_command(
        const at::Command &command,
        at::AtTransport &transport);
    void close_all();
    void set_scan_control(ScanPause pause, ScanResume resume, void *context);

private:
    struct Slot {
        int handle = -1;
        uint8_t *ca_certificate = nullptr;
        size_t ca_certificate_size = 0U;
        uint8_t *client_certificate = nullptr;
        size_t client_certificate_size = 0U;
        char client_private_key_slot[16] = {};
    };

    at::CommandResult handle_begin(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_set_ca(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_set_ecc_slot(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_connect(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_send(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_receive(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_close(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_simple_slot_command(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_state(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_diagnostic(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_verify_flags(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_endpoint(
        const at::Command &command,
        at::AtTransport &transport);

    bool parse_uint(
        const char *text,
        unsigned long maximum,
        unsigned long &value) const;
    bool parse_slot(
        const at::Command &command,
        size_t argument,
        size_t &slot) const;
    bool parse_ipv4(const char *text, uint8_t address[4]) const;
    bool slot_open(size_t slot) const;
    void close_slot(size_t slot);
    void release_ca(size_t slot);
    bool replace_ca(size_t slot, const uint8_t *data, size_t size);
    bool replace_client_credentials(
        size_t slot,
        const uint8_t *certificate,
        size_t certificate_size,
        const char *key_slot);
    void clear_client_credentials(size_t slot);
    static at::CommandResult set_ca_payload(
        void *context,
        const uint8_t *data,
        size_t size);
    static at::CommandResult set_ecc_slot_payload(
        void *context,
        const uint8_t *data,
        size_t size);
    static at::CommandResult send_payload(
        void *context,
        const uint8_t *data,
        size_t size);

    TlsClientBackend backend_;
    TlsPrivateKeyResolver private_key_resolver_ = nullptr;
    void *private_key_resolver_context_ = nullptr;
    ScanPause scan_pause_ = nullptr;
    ScanResume scan_resume_ = nullptr;
    void *scan_control_context_ = nullptr;
    Slot slots_[kTlsClientSlotCount] = {};
    size_t pending_ca_slot_ = kTlsClientSlotCount;
    size_t pending_ecc_slot_ = kTlsClientSlotCount;
    char pending_ecc_key_slot_[16] = {};
    size_t pending_send_slot_ = kTlsClientSlotCount;
};

const TlsClientBackend &bouffalo_tls_client_backend();

} // namespace bridge

#endif
