#include "tcp_client_service.h"
#include "network_scan_window.h"
#include "tcp_runtime_diagnostic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace bridge {

namespace {

constexpr char kTcpRxBudget[] = "+TCPRXBUDGET";
constexpr char kBeginClient[] = "+BEGINCLIENT";
constexpr char kClientState[] = "+CLIENTSTATE";
constexpr char kClientConnectIp[] = "+CLIENTCONNECTIP";
constexpr char kClientConnectName[] = "+CLIENTCONNECTNAME";
constexpr char kClientConnect[] = "+CLIENTCONNECT";
constexpr char kClientSend[] = "+CLIENTSEND";
constexpr char kClientReceive[] = "+CLIENTRECEIVE";
constexpr char kClientClose[] = "+CLIENTCLOSE";
constexpr char kClientConnected[] = "+CLIENTCONNECTED";
constexpr char kIpClient[] = "+IPCLIENT";
constexpr char kClientStatus[] = "+CLIENTSTATUS";
constexpr char kHostByName[] = "+HOSTBYNAME";
constexpr char kHostByNameDiagnostic[] = "+HOSTBYNAMEDIAG";
constexpr char kAvailable[] = "+AVAILABLE";
constexpr char kPeek[] = "+PEEK";
constexpr char kFlush[] = "+FLUSH";
constexpr char kRemoteIp[] = "+REMOTEIP";
constexpr char kRemotePort[] = "+REMOTEPORT";
constexpr uint32_t kDefaultConnectTimeoutMs = 3000U;

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

TcpClientService::TcpClientService(TcpClientBackend backend)
    : backend_(backend)
{
}

void TcpClientService::close_all()
{
    for (size_t slot = 0U; slot < kTcpClientSlotCount; ++slot) {
        close_slot(slot);
    }
}

void TcpClientService::set_scan_control(ScanPause pause, ScanResume resume, void *context)
{
    scan_pause_ = pause;
    scan_resume_ = resume;
    scan_control_context_ = context;
}

bool TcpClientService::handles_command(const at::Command &command) const
{
    return is_name(command, kTcpRxBudget) || is_name(command, kBeginClient) ||
        is_name(command, kClientState) ||
        is_name(command, kClientConnectIp) ||
        is_name(command, kClientConnectName) ||
        is_name(command, kClientConnect) ||
        is_name(command, kClientSend) ||
        is_name(command, kClientReceive) ||
        is_name(command, kClientClose) ||
        is_name(command, kClientConnected) ||
        is_name(command, kIpClient) ||
        is_name(command, kClientStatus) ||
        is_name(command, kHostByName) ||
        is_name(command, kHostByNameDiagnostic) ||
        is_name(command, kAvailable) ||
        is_name(command, kPeek) ||
        is_name(command, kFlush) ||
        is_name(command, kRemoteIp) ||
        is_name(command, kRemotePort);
}

at::CommandResult TcpClientService::handle_command(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (is_name(command, kTcpRxBudget)) {
        if (command.mode != at::CommandMode::Read || command.argument_count != 0U) {
            return at::CommandResult::Error;
        }
#if BL616CL_TCP_RX_BUDGET_DIAGNOSTIC
        TcpRxBudgetDiagnostic value;
        if (backend_.get_rx_budget_diagnostic == nullptr ||
            !backend_.get_rx_budget_diagnostic(backend_.context, &value)) {
            return at::CommandResult::Error;
        }
#endif
        write_prefix(transport, command);
        (void)transport.write_cstr("1,");
        write_u32(transport, BL616CL_TCP_RX_BUDGET_DIAGNOSTIC);
#if BL616CL_TCP_RX_BUDGET_DIAGNOSTIC
        for (uint32_t field : value.values) {
            (void)transport.write_cstr(",");
            write_u32(transport, field);
        }
#endif
        (void)transport.write_cstr("\r\n");
        return at::CommandResult::Ok;
    }
    if (is_name(command, kBeginClient)) {
        return handle_begin_client(command, transport);
    }
    if (is_name(command, kClientConnectIp) ||
        is_name(command, kClientConnectName) ||
        is_name(command, kClientConnect)) {
        return handle_connect(command, transport);
    }
    if (is_name(command, kClientSend)) {
        return handle_send(command, transport);
    }
    if (is_name(command, kClientReceive)) {
        return handle_receive(command, transport);
    }
    if (is_name(command, kClientClose)) {
        return handle_close(command, transport);
    }
    if (is_name(command, kClientState)) {
        return handle_state(command, transport);
    }
    if (is_name(command, kIpClient)) {
        size_t slot = 0U;
        uint8_t address[4] = {};
        bool connected = false;
        if (command.mode != at::CommandMode::Write ||
            command.argument_count != 1U ||
            !parse_slot(command, 0U, slot) ||
            !slot_open(slot) ||
            backend_.connected == nullptr ||
            !backend_.connected(backend_.context, slots_[slot].handle, &connected)) {
            return at::CommandResult::Error;
        }
        if (!connected) {
            return at::CommandResult::Ok;
        }
        if (
            backend_.get_local_address == nullptr ||
            !backend_.get_local_address(
                backend_.context, slots_[slot].handle, address)) {
            return at::CommandResult::Error;
        }
        write_prefix(transport, command);
        write_ip(transport, address);
        (void)transport.write_cstr("\r\n");
        return at::CommandResult::Ok;
    }
    if (is_name(command, kClientStatus)) {
        size_t slot = 0U;
        if (command.mode != at::CommandMode::Write ||
            command.argument_count != 1U ||
            !parse_slot(command, 0U, slot) ||
            !slot_open(slot)) {
            return at::CommandResult::Error;
        }
        (void)transport.write_response(command, "");
        return at::CommandResult::Ok;
    }
    if (is_name(command, kHostByName)) {
        return handle_host_by_name(command, transport);
    }
    if (is_name(command, kHostByNameDiagnostic)) {
        return handle_host_by_name_diagnostic(command, transport);
    }
    return handle_simple_slot_command(command, transport);
}

bool TcpClientService::parse_uint(
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

bool TcpClientService::parse_slot(
    const at::Command &command,
    size_t argument,
    size_t &slot) const
{
    if (argument >= command.argument_count) {
        return false;
    }
    unsigned long parsed = 0U;
    if (!parse_uint(command.arguments[argument], kTcpClientSlotCount - 1U, parsed)) {
        return false;
    }
    slot = static_cast<size_t>(parsed);
    return true;
}

bool TcpClientService::parse_ipv4(const char *text, uint8_t address[4]) const
{
    if (text == nullptr || address == nullptr) {
        return false;
    }
    unsigned int parts[4] = {};
    char tail = '\0';
    if (sscanf(
            text,
            "%u.%u.%u.%u%c",
            &parts[0],
            &parts[1],
            &parts[2],
            &parts[3],
            &tail) != 4) {
        return false;
    }
    for (size_t index = 0U; index < 4U; ++index) {
        if (parts[index] > 255U) {
            return false;
        }
        address[index] = static_cast<uint8_t>(parts[index]);
    }
    return true;
}

bool TcpClientService::valid_slot(size_t slot) const
{
    return slot < kTcpClientSlotCount;
}

bool TcpClientService::slot_open(size_t slot) const
{
    return valid_slot(slot) && slots_[slot].handle >= 0;
}

bool TcpClientService::open_slot(size_t slot)
{
    if (!valid_slot(slot) || slot_open(slot) || backend_.create == nullptr) {
        return false;
    }
    int handle = -1;
    if (!backend_.create(backend_.context, &handle) || handle < 0) {
        return false;
    }
    slots_[slot].handle = handle;
    return true;
}

void TcpClientService::close_slot(size_t slot)
{
    if (!slot_open(slot)) {
        return;
    }
    if (backend_.close != nullptr) {
        tcp_runtime_diagnostic_note_close_request(
            slots_[slot].handle,
            TcpCloseOrigin::TcpClientSlot);
        (void)backend_.close(backend_.context, slots_[slot].handle);
    }
    slots_[slot].handle = -1;
}

at::CommandResult TcpClientService::handle_begin_client(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode == at::CommandMode::Run && command.argument_count == 0U) {
        for (size_t slot = 0U; slot < kTcpClientSlotCount; ++slot) {
            if (open_slot(slot)) {
                write_prefix(transport, command);
                write_u32(transport, slot);
                (void)transport.write_cstr("\r\n");
                return at::CommandResult::Ok;
            }
        }
        return at::CommandResult::Error;
    }

    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U) {
        return at::CommandResult::Error;
    }
    size_t slot = 0U;
    if (!parse_slot(command, 0U, slot)) {
        return at::CommandResult::Error;
    }
    close_slot(slot);
    if (!open_slot(slot)) {
        return at::CommandResult::Error;
    }
    (void)transport.write_response(command, "");
    return at::CommandResult::Ok;
}

at::CommandResult TcpClientService::handle_connect(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count < 3U ||
        (strcmp(command.name, kClientConnect) == 0 &&
         command.argument_count > 4U) ||
        (strcmp(command.name, kClientConnect) != 0 &&
         command.argument_count != 3U)) {
        return at::CommandResult::Error;
    }

    size_t slot = 0U;
    if (!parse_slot(command, 0U, slot) || !slot_open(slot)) {
        return at::CommandResult::Error;
    }
    unsigned long port = 0U;
    if (!parse_uint(command.arguments[2], 65535U, port) || port == 0U) {
        return at::CommandResult::Error;
    }

    uint32_t timeout = kDefaultConnectTimeoutMs;
    if (strcmp(command.name, kClientConnect) == 0 && command.argument_count >= 4U) {
        unsigned long parsed_timeout = 0U;
        if (!parse_uint(command.arguments[3], 600000U, parsed_timeout)) {
            return at::CommandResult::Error;
        }
        timeout = static_cast<uint32_t>(parsed_timeout);
    }

    NetworkScanWindow scan_window(scan_pause_, scan_resume_, scan_control_context_);
    uint8_t address[4] = {};
    if (strcmp(command.name, kClientConnectIp) == 0) {
        if (!parse_ipv4(command.arguments[1], address)) {
            return at::CommandResult::Error;
        }
    } else if (backend_.resolve_hostname == nullptr ||
               !backend_.resolve_hostname(
                   backend_.context,
                   command.arguments[1],
                   address)) {
        return at::CommandResult::Error;
    }

    if (backend_.connect == nullptr ||
        !backend_.connect(
            backend_.context,
            slots_[slot].handle,
            address,
            static_cast<uint16_t>(port),
            timeout)) {
        return at::CommandResult::Error;
    }
    (void)transport.write_response(command, "");
    return at::CommandResult::Ok;
}

at::CommandResult TcpClientService::handle_send(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 2U) {
        return at::CommandResult::Error;
    }
    size_t slot = 0U;
    unsigned long size = 0U;
    if (!parse_slot(command, 0U, slot) ||
        !parse_uint(
            command.arguments[1],
            at::kTransportRawPayloadCapacity,
            size) ||
        size == 0U ||
        !slot_open(slot) ||
        backend_.send == nullptr) {
        return at::CommandResult::Error;
    }
    pending_send_slot_ = slot;
    if (!transport.request_raw_payload(
            static_cast<size_t>(size),
            &TcpClientService::send_payload,
            this)) {
        pending_send_slot_ = kTcpClientSlotCount;
        return at::CommandResult::Error;
    }
    return at::CommandResult::Deferred;
}

at::CommandResult TcpClientService::send_payload(
    void *context,
    const uint8_t *data,
    size_t size)
{
    TcpClientService *service = static_cast<TcpClientService *>(context);
    const size_t slot = service->pending_send_slot_;
    service->pending_send_slot_ = kTcpClientSlotCount;
    if (!service->slot_open(slot) || service->backend_.send == nullptr) {
        return at::CommandResult::Error;
    }
    size_t sent = 0U;
    if (!service->backend_.send(
            service->backend_.context,
            service->slots_[slot].handle,
            data,
            size,
            &sent) ||
        sent != size) {
        return at::CommandResult::Error;
    }
    return at::CommandResult::Ok;
}

at::CommandResult TcpClientService::handle_receive(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 2U) {
        return at::CommandResult::Error;
    }
    size_t slot = 0U;
    unsigned long requested = 0U;
    if (!parse_slot(command, 0U, slot) ||
        !parse_uint(
            command.arguments[1],
            at::kTransportRawPayloadCapacity,
            requested) ||
        requested == 0U ||
        !slot_open(slot) ||
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
            slots_[slot].handle,
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

at::CommandResult TcpClientService::handle_close(
    const at::Command &command,
    at::AtTransport &transport)
{
    size_t slot = 0U;
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        !parse_slot(command, 0U, slot)) {
        return at::CommandResult::Error;
    }
    close_slot(slot);
    (void)transport.write_response(command, "");
    return at::CommandResult::Ok;
}

at::CommandResult TcpClientService::handle_state(
    const at::Command &command,
    at::AtTransport &transport)
{
    size_t slot = 0U;
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        !parse_slot(command, 0U, slot) ||
        !slot_open(slot) ||
        backend_.get_state == nullptr) {
        return at::CommandResult::Error;
    }
    bool connected = false;
    if (backend_.connected == nullptr ||
        !backend_.connected(backend_.context, slots_[slot].handle, &connected)) {
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
            slots_[slot].handle,
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

at::CommandResult TcpClientService::handle_host_by_name(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        command.arguments[0][0] == '\0' ||
        backend_.resolve_hostname == nullptr) {
        return at::CommandResult::Error;
    }
    uint8_t address[4] = {};
    if (!backend_.resolve_hostname(
            backend_.context,
            command.arguments[0],
            address)) {
        return at::CommandResult::Error;
    }
    write_prefix(transport, command);
    write_ip(transport, address);
    (void)transport.write_cstr("\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult TcpClientService::handle_host_by_name_diagnostic(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Run ||
        command.argument_count != 0U ||
        backend_.get_hostname_diagnostic == nullptr) {
        return at::CommandResult::Error;
    }
    TcpHostnameDiagnostic diagnostic = {};
    if (!backend_.get_hostname_diagnostic(
            backend_.context,
            &diagnostic)) {
        return at::CommandResult::Error;
    }
    write_prefix(transport, command);
    write_u32(transport, (diagnostic.flags & 1U) != 0U ? 1U : 0U);
    (void)transport.write_cstr(",");
    write_u32(transport, (diagnostic.flags & 2U) != 0U ? 1U : 0U);
    (void)transport.write_cstr(",");
    write_i32(transport, diagnostic.h_error);
    (void)transport.write_cstr(",");
    write_i32(transport, diagnostic.raw_error);
    (void)transport.write_cstr(",");
    write_ip(transport, diagnostic.server_address);
    (void)transport.write_cstr("\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult TcpClientService::handle_simple_slot_command(
    const at::Command &command,
    at::AtTransport &transport)
{
    size_t slot = 0U;
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        !parse_slot(command, 0U, slot) ||
        !slot_open(slot)) {
        return at::CommandResult::Error;
    }

    if (is_name(command, kAvailable)) {
        size_t bytes = 0U;
        if (backend_.available == nullptr ||
            !backend_.available(backend_.context, slots_[slot].handle, &bytes)) {
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
            !backend_.peek(backend_.context, slots_[slot].handle, &value)) {
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
            !backend_.connected(backend_.context, slots_[slot].handle, &connected)) {
            return at::CommandResult::Error;
        }
        (void)transport.write_response(command, connected ? "1" : "0");
        return at::CommandResult::Ok;
    }

    if (is_name(command, kFlush)) {
        if (backend_.flush == nullptr ||
            !backend_.flush(backend_.context, slots_[slot].handle)) {
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
                slots_[slot].handle,
                address,
                &remote_port,
                &local_port)) {
            return at::CommandResult::Error;
        }
        write_prefix(transport, command);
        if (is_name(command, kRemoteIp)) {
            write_ip(transport, address);
        } else if (is_name(command, kRemotePort)) {
            write_u32(transport, remote_port);
        }
        (void)transport.write_cstr("\r\n");
        return at::CommandResult::Ok;
    }

    return at::CommandResult::Error;
}

} // namespace bridge
