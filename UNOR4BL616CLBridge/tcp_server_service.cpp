#include "tcp_server_service.h"
#include "tcp_runtime_diagnostic.h"
#include "network_scan_window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace bridge {

namespace {

constexpr char kBeginServer[] = "+BEGINSERVER";
constexpr char kServerAvailable[] = "+SERVERAVAILABLE";
constexpr char kServerAccept[] = "+SERVERACCEPT";
constexpr char kServerWrite[] = "+SERVERWRITE";
constexpr char kServerEnd[] = "+SERVEREND";
constexpr char kClientState[] = "+CLIENTSTATE";
constexpr char kClientSend[] = "+CLIENTSEND";
constexpr char kClientReceive[] = "+CLIENTRECEIVE";
constexpr char kClientClose[] = "+CLIENTCLOSE";
constexpr char kClientConnected[] = "+CLIENTCONNECTED";
constexpr char kIpClient[] = "+IPCLIENT";
constexpr char kClientStatus[] = "+CLIENTSTATUS";
constexpr char kAvailable[] = "+AVAILABLE";
constexpr char kPeek[] = "+PEEK";
constexpr char kFlush[] = "+FLUSH";
constexpr char kRemoteIp[] = "+REMOTEIP";
constexpr char kRemotePort[] = "+REMOTEPORT";

bool is_name(const at::Command &command, const char *name)
{
    return strcmp(command.name, name) == 0;
}

void write_u32(at::AtTransport &transport, unsigned long value)
{
    char text[16] = {};
    const int size = snprintf(text, sizeof(text), "%lu", value);
    if (size > 0) {
        (void)transport.write_cstr(text);
    }
}

void write_i32(at::AtTransport &transport, int value)
{
    char text[16] = {};
    const int size = snprintf(text, sizeof(text), "%d", value);
    if (size > 0) {
        (void)transport.write_cstr(text);
    }
}

void write_ip(at::AtTransport &transport, const uint8_t address[4])
{
    char text[16] = {};
    const int size = snprintf(
        text,
        sizeof(text),
        "%u.%u.%u.%u",
        static_cast<unsigned int>(address[0]),
        static_cast<unsigned int>(address[1]),
        static_cast<unsigned int>(address[2]),
        static_cast<unsigned int>(address[3]));
    if (size > 0) {
        (void)transport.write_cstr(text);
    }
}

void write_prefix(at::AtTransport &transport, const at::Command &command)
{
    (void)transport.write_cstr(command.name);
    (void)transport.write_cstr(": ");
}

} // namespace

TcpServerService::TcpServerService(TcpClientBackend backend)
    : backend_(backend)
{
}

void TcpServerService::set_scan_control(TcpClientService::ScanPause pause,
                                      TcpClientService::ScanResume resume, void *context)
{
    scan_pause_ = pause;
    scan_resume_ = resume;
    scan_control_context_ = context;
}

void TcpServerService::close_all(TcpCloseOrigin origin)
{
    for (size_t slot = 0U; slot < kTcpServerSlotCount; ++slot) {
        close_server_slot(slot, origin);
    }
    for (size_t slot = 0U; slot < kTcpServerClientSlotCount; ++slot) {
        close_client_slot(slot);
    }
}

bool TcpServerService::handles_server_command(const at::Command &command) const
{
    return is_name(command, kBeginServer) ||
        is_name(command, kServerAvailable) ||
        is_name(command, kServerAccept) ||
        is_name(command, kServerWrite) ||
        is_name(command, kServerEnd);
}

bool TcpServerService::handles_client_command(const at::Command &command) const
{
    if (!(is_name(command, kClientState) ||
          is_name(command, kClientSend) ||
          is_name(command, kClientReceive) ||
          is_name(command, kClientClose) ||
          is_name(command, kClientConnected) ||
          is_name(command, kIpClient) ||
          is_name(command, kClientStatus) ||
          is_name(command, kAvailable) ||
          is_name(command, kPeek) ||
          is_name(command, kFlush) ||
          is_name(command, kRemoteIp) ||
          is_name(command, kRemotePort))) {
        return false;
    }

    size_t slot = 0U;
    return parse_client_slot(command, 0U, slot);
}

bool TcpServerService::parse_uint(
    const char *text,
    unsigned long maximum,
    unsigned long &value) const
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    char *end = nullptr;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed > maximum) {
        return false;
    }
    value = parsed;
    return true;
}

bool TcpServerService::parse_server_slot(
    const at::Command &command,
    size_t argument,
    size_t &slot) const
{
    if (argument >= command.argument_count) {
        return false;
    }
    unsigned long parsed = 0U;
    if (!parse_uint(command.arguments[argument], kTcpServerSlotCount - 1U, parsed)) {
        return false;
    }
    slot = static_cast<size_t>(parsed);
    return true;
}

bool TcpServerService::parse_client_slot(
    const at::Command &command,
    size_t argument,
    size_t &slot) const
{
    if (argument >= command.argument_count) {
        return false;
    }
    unsigned long parsed = 0U;
    if (!parse_uint(
            command.arguments[argument],
            kTcpServerClientSocketBase + kTcpServerClientSlotCount - 1U,
            parsed) ||
        parsed < kTcpServerClientSocketBase) {
        return false;
    }
    slot = static_cast<size_t>(parsed - kTcpServerClientSocketBase);
    return true;
}

bool TcpServerService::server_open(size_t slot) const
{
    return slot < kTcpServerSlotCount && servers_[slot].listener_handle >= 0;
}

bool TcpServerService::client_open(size_t slot) const
{
    return slot < kTcpServerClientSlotCount && clients_[slot].handle >= 0;
}

bool TcpServerService::accept_one(
    size_t server_slot,
    bool accepted,
    size_t *client_slot)
{
    if (!server_open(server_slot) || backend_.accept == nullptr) {
        return false;
    }

    size_t free_slot = kTcpServerClientSlotCount;
    for (size_t slot = 0U; slot < kTcpServerClientSlotCount; ++slot) {
        if (!client_open(slot)) {
            free_slot = slot;
            break;
        }
    }
    if (free_slot == kTcpServerClientSlotCount) {
        return false;
    }

    int handle = -1;
    NetworkScanWindow scan_window(scan_pause_, scan_resume_, scan_control_context_);
    if (!backend_.accept(
            backend_.context,
            servers_[server_slot].listener_handle,
            &handle)) {
        return false;
    }
    if (handle < 0) {
        return true;
    }

    clients_[free_slot].handle = handle;
    clients_[free_slot].server_slot = server_slot;
    clients_[free_slot].accepted = accepted;
    if (client_slot != nullptr) {
        *client_slot = free_slot;
    }
    return true;
}

void TcpServerService::accept_pending(size_t server_slot)
{
    while (true) {
        size_t client_slot = kTcpServerClientSlotCount;
        if (!accept_one(server_slot, false, &client_slot) ||
            client_slot == kTcpServerClientSlotCount) {
            return;
        }
    }
}

void TcpServerService::close_client_slot(size_t slot)
{
    if (!client_open(slot)) {
        return;
    }
    if (backend_.close != nullptr) {
        tcp_runtime_diagnostic_note_close_request(
            clients_[slot].handle,
            TcpCloseOrigin::TcpServerClientSlot);
        (void)backend_.close(backend_.context, clients_[slot].handle);
    }
    clients_[slot] = {};
}

void TcpServerService::close_server_slot(size_t slot, TcpCloseOrigin origin)
{
    if (!server_open(slot)) {
        return;
    }
    for (size_t client_slot = 0U; client_slot < kTcpServerClientSlotCount; ++client_slot) {
        if (client_open(client_slot) && clients_[client_slot].server_slot == slot) {
            close_client_slot(client_slot);
        }
    }
    if (backend_.close != nullptr) {
        tcp_runtime_diagnostic_note_close_request(
            servers_[slot].listener_handle,
            origin);
        (void)backend_.close(backend_.context, servers_[slot].listener_handle);
    }
    servers_[slot] = {};
}

at::CommandResult TcpServerService::handle_server_command(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (is_name(command, kBeginServer)) {
        return handle_begin_server(command, transport);
    }
    if (is_name(command, kServerAvailable)) {
        return handle_server_available(command, transport);
    }
    if (is_name(command, kServerAccept)) {
        return handle_server_accept(command, transport);
    }
    if (is_name(command, kServerWrite)) {
        return handle_server_write(command, transport);
    }
    return handle_server_end(command, transport);
}

at::CommandResult TcpServerService::handle_begin_server(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        backend_.create_listener == nullptr) {
        return at::CommandResult::Error;
    }
    unsigned long port = 0U;
    if (!parse_uint(command.arguments[0], 65535U, port) || port == 0U) {
        return at::CommandResult::Error;
    }
    for (size_t slot = 0U; slot < kTcpServerSlotCount; ++slot) {
        if (server_open(slot)) {
            continue;
        }
        int handle = -1;
        NetworkScanWindow scan_window(scan_pause_, scan_resume_, scan_control_context_);
        if (!backend_.create_listener(
                backend_.context,
                static_cast<uint16_t>(port),
                &handle) ||
            handle < 0) {
            return at::CommandResult::Error;
        }
        servers_[slot].listener_handle = handle;
        write_prefix(transport, command);
        write_u32(transport, slot);
        (void)transport.write_cstr("\r\n");
        return at::CommandResult::Ok;
    }
    return at::CommandResult::Error;
}

at::CommandResult TcpServerService::handle_server_available(
    const at::Command &command,
    at::AtTransport &transport)
{
    size_t server_slot = 0U;
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        !parse_server_slot(command, 0U, server_slot) ||
        !server_open(server_slot)) {
        return at::CommandResult::Error;
    }

    accept_pending(server_slot);
    int client_handle = -1;
    for (size_t checked = 0U; checked < kTcpServerClientSlotCount; ++checked) {
        const size_t slot =
            (last_available_client_slot_ + checked) % kTcpServerClientSlotCount;
        if (!client_open(slot) ||
            clients_[slot].server_slot != server_slot ||
            clients_[slot].accepted ||
            backend_.available == nullptr) {
            continue;
        }
        size_t bytes = 0U;
        if (!backend_.available(backend_.context, clients_[slot].handle, &bytes)) {
            continue;
        }
        if (bytes != 0U) {
            client_handle = static_cast<int>(kTcpServerClientSocketBase + slot);
            last_available_client_slot_ = slot;
            break;
        }
    }

    write_prefix(transport, command);
    write_i32(transport, client_handle);
    (void)transport.write_cstr("\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult TcpServerService::handle_server_accept(
    const at::Command &command,
    at::AtTransport &transport)
{
    size_t server_slot = 0U;
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        !parse_server_slot(command, 0U, server_slot) ||
        !server_open(server_slot)) {
        return at::CommandResult::Error;
    }

    size_t client_slot = kTcpServerClientSlotCount;
    if (!accept_one(server_slot, true, &client_slot)) {
        return at::CommandResult::Error;
    }
    const int client_handle = client_slot == kTcpServerClientSlotCount
        ? -1
        : static_cast<int>(kTcpServerClientSocketBase + client_slot);
    write_prefix(transport, command);
    write_i32(transport, client_handle);
    (void)transport.write_cstr("\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult TcpServerService::handle_server_write(
    const at::Command &command,
    at::AtTransport &transport)
{
    size_t server_slot = 0U;
    unsigned long size = 0U;
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 2U ||
        !parse_server_slot(command, 0U, server_slot) ||
        !server_open(server_slot) ||
        !parse_uint(
            command.arguments[1],
            at::kTransportRawPayloadCapacity,
            size) ||
        size == 0U) {
        return at::CommandResult::Error;
    }
    pending_server_write_slot_ = server_slot;
    if (!transport.request_raw_payload(
            static_cast<size_t>(size),
            &TcpServerService::send_server_payload,
            this)) {
        pending_server_write_slot_ = kTcpServerSlotCount;
        return at::CommandResult::Error;
    }
    return at::CommandResult::Deferred;
}

at::CommandResult TcpServerService::send_server_payload(
    void *context,
    const uint8_t *data,
    size_t size)
{
    TcpServerService *service = static_cast<TcpServerService *>(context);
    const size_t server_slot = service->pending_server_write_slot_;
    service->pending_server_write_slot_ = kTcpServerSlotCount;
    if (!service->server_open(server_slot) || service->backend_.send == nullptr) {
        return at::CommandResult::Error;
    }
    for (size_t slot = 0U; slot < kTcpServerClientSlotCount; ++slot) {
        if (!service->client_open(slot) ||
            service->clients_[slot].server_slot != server_slot ||
            service->clients_[slot].accepted) {
            continue;
        }
        size_t sent = 0U;
        (void)service->backend_.send(
            service->backend_.context,
            service->clients_[slot].handle,
            data,
            size,
            &sent);
    }
    return at::CommandResult::Ok;
}

at::CommandResult TcpServerService::handle_server_end(
    const at::Command &command,
    at::AtTransport &transport)
{
    size_t server_slot = 0U;
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        !parse_server_slot(command, 0U, server_slot) ||
        !server_open(server_slot)) {
        return at::CommandResult::Error;
    }
    close_server_slot(server_slot, TcpCloseOrigin::TcpServerEnd);
    (void)transport.write_response(command, "");
    return at::CommandResult::Ok;
}

at::CommandResult TcpServerService::handle_client_command(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (is_name(command, kClientSend)) {
        return handle_client_send(command, transport);
    }
    if (is_name(command, kClientReceive)) {
        return handle_client_receive(command, transport);
    }
    if (is_name(command, kClientClose)) {
        return handle_client_close(command, transport);
    }
    if (is_name(command, kClientState)) {
        return handle_client_state(command, transport);
    }
    return handle_simple_client_command(command, transport);
}

at::CommandResult TcpServerService::handle_client_send(
    const at::Command &command,
    at::AtTransport &transport)
{
    size_t client_slot = 0U;
    unsigned long size = 0U;
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 2U ||
        !parse_client_slot(command, 0U, client_slot) ||
        !client_open(client_slot) ||
        !parse_uint(
            command.arguments[1],
            at::kTransportRawPayloadCapacity,
            size) ||
        size == 0U ||
        backend_.send == nullptr) {
        return at::CommandResult::Error;
    }
    pending_client_send_slot_ = client_slot;
    if (!transport.request_raw_payload(
            static_cast<size_t>(size),
            &TcpServerService::send_client_payload,
            this)) {
        pending_client_send_slot_ = kTcpServerClientSlotCount;
        return at::CommandResult::Error;
    }
    return at::CommandResult::Deferred;
}

at::CommandResult TcpServerService::send_client_payload(
    void *context,
    const uint8_t *data,
    size_t size)
{
    TcpServerService *service = static_cast<TcpServerService *>(context);
    const size_t client_slot = service->pending_client_send_slot_;
    service->pending_client_send_slot_ = kTcpServerClientSlotCount;
    if (!service->client_open(client_slot) || service->backend_.send == nullptr) {
        return at::CommandResult::Error;
    }
    size_t sent = 0U;
    if (!service->backend_.send(
            service->backend_.context,
            service->clients_[client_slot].handle,
            data,
            size,
            &sent) ||
        sent != size) {
        return at::CommandResult::Error;
    }
    return at::CommandResult::Ok;
}

at::CommandResult TcpServerService::handle_client_receive(
    const at::Command &command,
    at::AtTransport &transport)
{
    size_t client_slot = 0U;
    unsigned long requested = 0U;
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 2U ||
        !parse_client_slot(command, 0U, client_slot) ||
        !client_open(client_slot) ||
        !parse_uint(
            command.arguments[1],
            at::kTransportRawPayloadCapacity,
            requested) ||
        requested == 0U ||
        backend_.receive == nullptr) {
        return at::CommandResult::Error;
    }
    uint8_t *receive_buffer =
        transport.temporary_workspace(static_cast<size_t>(requested));
    if (receive_buffer == nullptr) {
        return at::CommandResult::Error;
    }
    size_t received = 0U;
    if (!backend_.receive(
            backend_.context,
            clients_[client_slot].handle,
            receive_buffer,
            static_cast<size_t>(requested),
            &received) ||
        received > requested) {
        return at::CommandResult::Error;
    }
    write_prefix(transport, command);
    write_u32(transport, received);
    (void)transport.write_cstr("|");
    if (received != 0U) {
        (void)transport.write_borrowed_data(receive_buffer, received);
    }
    return at::CommandResult::Ok;
}

at::CommandResult TcpServerService::handle_client_close(
    const at::Command &command,
    at::AtTransport &transport)
{
    size_t client_slot = 0U;
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        !parse_client_slot(command, 0U, client_slot) ||
        !client_open(client_slot)) {
        return at::CommandResult::Error;
    }
    close_client_slot(client_slot);
    (void)transport.write_response(command, "");
    return at::CommandResult::Ok;
}

at::CommandResult TcpServerService::handle_client_state(
    const at::Command &command,
    at::AtTransport &transport)
{
    size_t client_slot = 0U;
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        !parse_client_slot(command, 0U, client_slot) ||
        !client_open(client_slot) ||
        backend_.get_state == nullptr) {
        return at::CommandResult::Error;
    }
    bool connected = false;
    if (backend_.connected == nullptr ||
        !backend_.connected(backend_.context, clients_[client_slot].handle, &connected)) {
        return at::CommandResult::Error;
    }
    if (!connected) {
        return at::CommandResult::Ok;
    }
    uint8_t address[4] = {};
    uint16_t remote_port = 0U;
    uint16_t local_port = 0U;
    if (!backend_.get_state(
            backend_.context,
            clients_[client_slot].handle,
            address,
            &remote_port,
            &local_port)) {
        return at::CommandResult::Error;
    }
    write_prefix(transport, command);
    write_ip(transport, address);
    (void)transport.write_cstr(",");
    write_u32(transport, remote_port);
    (void)transport.write_cstr(",");
    write_u32(transport, local_port);
    (void)transport.write_cstr("\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult TcpServerService::handle_simple_client_command(
    const at::Command &command,
    at::AtTransport &transport)
{
    size_t client_slot = 0U;
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        !parse_client_slot(command, 0U, client_slot) ||
        !client_open(client_slot)) {
        return at::CommandResult::Error;
    }

    if (is_name(command, kAvailable)) {
        size_t bytes = 0U;
        if (backend_.available == nullptr ||
            !backend_.available(backend_.context, clients_[client_slot].handle, &bytes)) {
            return at::CommandResult::Error;
        }
        write_prefix(transport, command);
        write_u32(transport, bytes);
        (void)transport.write_cstr("\r\n");
        return at::CommandResult::Ok;
    }

    if (is_name(command, kPeek)) {
        int value = -1;
        if (backend_.peek == nullptr ||
            !backend_.peek(backend_.context, clients_[client_slot].handle, &value)) {
            return at::CommandResult::Error;
        }
        write_prefix(transport, command);
        write_i32(transport, value);
        (void)transport.write_cstr("\r\n");
        return at::CommandResult::Ok;
    }

    if (is_name(command, kClientConnected)) {
        bool connected = false;
        if (backend_.connected == nullptr ||
            !backend_.connected(backend_.context, clients_[client_slot].handle, &connected)) {
            return at::CommandResult::Error;
        }
        (void)transport.write_response(command, connected ? "1" : "0");
        return at::CommandResult::Ok;
    }

    if (is_name(command, kClientStatus)) {
        (void)transport.write_response(command, "");
        return at::CommandResult::Ok;
    }

    if (is_name(command, kIpClient)) {
        bool connected = false;
        if (backend_.connected == nullptr ||
            !backend_.connected(backend_.context, clients_[client_slot].handle, &connected)) {
            return at::CommandResult::Error;
        }
        if (!connected) {
            return at::CommandResult::Ok;
        }
        uint8_t address[4] = {};
        if (backend_.get_local_address == nullptr ||
            !backend_.get_local_address(
                backend_.context, clients_[client_slot].handle, address)) {
            return at::CommandResult::Error;
        }
        write_prefix(transport, command);
        write_ip(transport, address);
        (void)transport.write_cstr("\r\n");
        return at::CommandResult::Ok;
    }

    if (is_name(command, kFlush)) {
        if (backend_.flush == nullptr ||
            !backend_.flush(backend_.context, clients_[client_slot].handle)) {
            return at::CommandResult::Error;
        }
        write_prefix(transport, command);
        (void)transport.write_cstr("\r\n");
        return at::CommandResult::Ok;
    }

    if (is_name(command, kRemoteIp) || is_name(command, kRemotePort)) {
        uint8_t address[4] = {};
        uint16_t remote_port = 0U;
        uint16_t local_port = 0U;
        if (backend_.get_state == nullptr ||
            !backend_.get_state(
                backend_.context,
                clients_[client_slot].handle,
                address,
                &remote_port,
                &local_port)) {
            return at::CommandResult::Error;
        }
        write_prefix(transport, command);
        if (is_name(command, kRemoteIp)) {
            write_ip(transport, address);
        } else {
            write_u32(transport, remote_port);
        }
        (void)transport.write_cstr("\r\n");
        return at::CommandResult::Ok;
    }

    return at::CommandResult::Error;
}

} // namespace bridge
