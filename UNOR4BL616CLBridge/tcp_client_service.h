#ifndef UNOR4_BL616CL_TCP_CLIENT_SERVICE_H
#define UNOR4_BL616CL_TCP_CLIENT_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "at_transport.h"
#include "tcp_rx_budget_diagnostic.h"

namespace bridge {

constexpr size_t kTcpClientSlotCount = 8U;

struct TcpHostnameDiagnostic {
    uint8_t server_address[4] = {};
    int16_t h_error = 0;
    int16_t raw_error = 0;
    uint8_t flags = 0U;
};

struct TcpClientBackend {
    void *context = nullptr;
    bool (*create)(void *context, int *handle) = nullptr;
    bool (*close)(void *context, int handle) = nullptr;
    bool (*create_listener)(
        void *context,
        uint16_t port,
        int *handle) = nullptr;
    bool (*accept)(
        void *context,
        int listener_handle,
        int *client_handle) = nullptr;
    bool (*resolve_hostname)(
        void *context,
        const char *hostname,
        uint8_t address[4]) = nullptr;
    bool (*get_hostname_diagnostic)(
        void *context,
        TcpHostnameDiagnostic *diagnostic) = nullptr;
    bool (*connect)(
        void *context,
        int handle,
        const uint8_t address[4],
        uint16_t port,
        uint32_t timeout_ms) = nullptr;
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
    bool (*get_local_address)(
        void *context,
        int handle,
        uint8_t local_address[4]) = nullptr;
#if BL616CL_TCP_RX_BUDGET_DIAGNOSTIC
    bool (*get_rx_budget_diagnostic)(void *context, TcpRxBudgetDiagnostic *value) = nullptr;
#endif
};

class TcpClientService {
public:
    using ScanPause = bool (*)(void *context);
    using ScanResume = void (*)(void *context);
    explicit TcpClientService(TcpClientBackend backend = {});

    bool handles_command(const at::Command &command) const;
    at::CommandResult handle_command(
        const at::Command &command,
        at::AtTransport &transport);
    void close_all();
    void set_scan_control(ScanPause pause, ScanResume resume, void *context);

private:
    struct Slot {
        int handle = -1;
    };

    at::CommandResult handle_begin_client(
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
    at::CommandResult handle_host_by_name(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_host_by_name_diagnostic(
        const at::Command &command,
        at::AtTransport &transport);

    bool parse_slot(const at::Command &command, size_t argument, size_t &slot) const;
    bool parse_uint(
        const char *text,
        unsigned long maximum,
        unsigned long &value) const;
    bool parse_ipv4(const char *text, uint8_t address[4]) const;
    bool valid_slot(size_t slot) const;
    bool slot_open(size_t slot) const;
    bool open_slot(size_t slot);
    void close_slot(size_t slot);
    static at::CommandResult send_payload(
        void *context,
        const uint8_t *data,
        size_t size);

    TcpClientBackend backend_;
    ScanPause scan_pause_ = nullptr;
    ScanResume scan_resume_ = nullptr;
    void *scan_control_context_ = nullptr;
    Slot slots_[kTcpClientSlotCount] = {};
    size_t pending_send_slot_ = kTcpClientSlotCount;
};

const TcpClientBackend &bouffalo_tcp_client_backend();

} // namespace bridge

#endif
