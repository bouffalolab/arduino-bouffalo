#ifndef UNOR4_BL616CL_TCP_SERVER_SERVICE_H
#define UNOR4_BL616CL_TCP_SERVER_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "at_transport.h"
#include "tcp_client_service.h"
#include "tcp_runtime_diagnostic.h"

namespace bridge {

constexpr size_t kTcpServerSlotCount = 4U;
constexpr size_t kTcpServerClientSlotCount = 8U;
constexpr unsigned long kTcpServerClientSocketBase = 1000U;

class TcpServerService {
public:
    explicit TcpServerService(TcpClientBackend backend = {});
    void set_scan_control(TcpClientService::ScanPause pause,
                          TcpClientService::ScanResume resume, void *context);

    bool handles_server_command(const at::Command &command) const;
    bool handles_client_command(const at::Command &command) const;
    at::CommandResult handle_server_command(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_client_command(
        const at::Command &command,
        at::AtTransport &transport);
    void close_all(TcpCloseOrigin origin = TcpCloseOrigin::SoftResetWifi);

private:
    struct ServerSlot {
        int listener_handle = -1;
    };

    struct ClientSlot {
        int handle = -1;
        size_t server_slot = kTcpServerSlotCount;
        bool accepted = false;
    };

    at::CommandResult handle_begin_server(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_server_available(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_server_accept(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_server_write(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_server_end(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_client_send(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_client_receive(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_client_close(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_client_state(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_simple_client_command(
        const at::Command &command,
        at::AtTransport &transport);

    bool parse_uint(
        const char *text,
        unsigned long maximum,
        unsigned long &value) const;
    bool parse_server_slot(
        const at::Command &command,
        size_t argument,
        size_t &slot) const;
    bool parse_client_slot(
        const at::Command &command,
        size_t argument,
        size_t &slot) const;
    bool server_open(size_t slot) const;
    bool client_open(size_t slot) const;
    bool accept_one(size_t server_slot, bool accepted, size_t *client_slot);
    void accept_pending(size_t server_slot);
    void close_client_slot(size_t slot);
    void close_server_slot(size_t slot, TcpCloseOrigin origin);
    static at::CommandResult send_server_payload(
        void *context,
        const uint8_t *data,
        size_t size);
    static at::CommandResult send_client_payload(
        void *context,
        const uint8_t *data,
        size_t size);

    TcpClientBackend backend_;
    TcpClientService::ScanPause scan_pause_ = nullptr;
    TcpClientService::ScanResume scan_resume_ = nullptr;
    void *scan_control_context_ = nullptr;
    ServerSlot servers_[kTcpServerSlotCount] = {};
    ClientSlot clients_[kTcpServerClientSlotCount] = {};
    size_t last_available_client_slot_ = 0U;
    size_t pending_server_write_slot_ = kTcpServerSlotCount;
    size_t pending_client_send_slot_ = kTcpServerClientSlotCount;
};

} // namespace bridge

#endif
