#include "udp_service.h"
#if defined(ARDUINO)
#include "udp_socket_diagnostic.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace bridge {

namespace {

constexpr char kUdpBegin[] = "+UDPBEGIN";
constexpr char kUdpBeginMulti[] = "+UDPBEGINMULTI";
constexpr char kUdpBeginPacket[] = "+UDPBEGINPACKET";
constexpr char kBeginPacketMulti[] = "+BEGINPACKETMULTI";
constexpr char kUdpBeginPacketIp[] = "+UDPBEGINPACKETIP";
constexpr char kUdpBeginPacketAdd[] = "+UDPBEGINPACKETADD";
constexpr char kLegacyBeginPacketAdd[] = "+BEGINPACKETADD";
constexpr char kUdpWrite[] = "+UDPWRITE";
constexpr char kUdpEndPacket[] = "+UDPENDPACKET";
constexpr char kUdpParse[] = "+UDPPARSE";
constexpr char kUdpAvailable[] = "+UDPAVAILABLE";
constexpr char kUdpRead[] = "+UDPREAD";
constexpr char kUdpPeek[] = "+UDPPEEK";
constexpr char kUdpFlush[] = "+UDPFLUSH";
constexpr char kUdpRemoteIp[] = "+UDPREMOTEIP";
constexpr char kUdpRemotePort[] = "+UDPREMOTEPORT";
constexpr char kUdpDebug[] = "+UDPDEBUG";
constexpr char kUdpStop[] = "+UDPSTOP";

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

void write_hex_ip(at::AtTransport &transport, const uint8_t address[4])
{
    char text[9] = {};
    const int size = snprintf(
        text,
        sizeof(text),
        "%02X%02X%02X%02X",
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

UdpService::UdpService(UdpBackend backend)
    : backend_(backend)
{
}

UdpService::~UdpService()
{
    close_all();
}

void UdpService::set_scan_control(NetworkScanWindow::Pause pause,
                                 NetworkScanWindow::Resume resume, void *context)
{
    scan_pause_ = pause;
    scan_resume_ = resume;
    scan_control_context_ = context;
}

bool UdpService::handles_command(const at::Command &command) const
{
    return is_name(command, kUdpBegin) ||
        is_name(command, kUdpBeginMulti) ||
        is_name(command, kUdpBeginPacket) ||
        is_name(command, kBeginPacketMulti) ||
        is_name(command, kUdpBeginPacketIp) ||
        is_name(command, kUdpBeginPacketAdd) ||
        is_name(command, kLegacyBeginPacketAdd) ||
        is_name(command, kUdpWrite) ||
        is_name(command, kUdpEndPacket) ||
        is_name(command, kUdpParse) ||
        is_name(command, kUdpAvailable) ||
        is_name(command, kUdpRead) ||
        is_name(command, kUdpPeek) ||
        is_name(command, kUdpFlush) ||
        is_name(command, kUdpRemoteIp) ||
        is_name(command, kUdpRemotePort) ||
        is_name(command, kUdpDebug) ||
        is_name(command, kUdpStop);
}

at::CommandResult UdpService::handle_command(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (is_name(command, kUdpBegin)) {
        return handle_begin(command, transport);
    }
    if (is_name(command, kUdpBeginMulti)) {
        return handle_begin_multicast(command, transport);
    }
    if (is_name(command, kUdpBeginPacket)) {
        return handle_begin_packet(command, transport);
    }
    if (is_name(command, kBeginPacketMulti)) {
        return handle_begin_packet_multicast(command, transport);
    }
    if (is_name(command, kUdpBeginPacketIp)) {
        return handle_begin_packet_ip(command, transport);
    }
    if (is_name(command, kUdpBeginPacketAdd) ||
        is_name(command, kLegacyBeginPacketAdd)) {
        return handle_begin_packet_name(command, transport);
    }
    if (is_name(command, kUdpWrite)) {
        return handle_write(command, transport);
    }
    if (is_name(command, kUdpEndPacket)) {
        return handle_end_packet(command, transport);
    }
    if (is_name(command, kUdpParse)) {
        return handle_parse(command, transport);
    }
    if (is_name(command, kUdpAvailable)) {
        return handle_available(command, transport);
    }
    if (is_name(command, kUdpRead)) {
        return handle_read(command, transport);
    }
    if (is_name(command, kUdpPeek)) {
        return handle_peek(command, transport);
    }
    if (is_name(command, kUdpFlush)) {
        return handle_flush(command, transport);
    }
    if (is_name(command, kUdpRemoteIp)) {
        return handle_remote_ip(command, transport);
    }
    if (is_name(command, kUdpRemotePort)) {
        return handle_remote_port(command, transport);
    }
    if (is_name(command, kUdpDebug)) {
        return handle_debug(command, transport);
    }
    return handle_stop(command, transport);
}

bool UdpService::parse_uint(
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

bool UdpService::parse_slot(
    const at::Command &command,
    size_t argument,
    size_t &slot) const
{
    if (argument >= command.argument_count) {
        return false;
    }
    unsigned long parsed = 0U;
    if (!parse_uint(command.arguments[argument], kUdpSlotCount - 1U, parsed)) {
        return false;
    }
    slot = static_cast<size_t>(parsed);
    return true;
}

bool UdpService::parse_ipv4(const char *text, uint8_t address[4])
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

bool UdpService::slot_open(size_t slot) const
{
    return slot < kUdpSlotCount && slots_[slot].handle >= 0;
}

void UdpService::discard_received(size_t slot)
{
    if (slot >= kUdpSlotCount) {
        return;
    }
    slots_[slot].received = nullptr;
    slots_[slot].received_size = 0U;
    slots_[slot].received_position = 0U;
    if (received_storage_slot_ == slot) {
        received_storage_slot_ = kUdpSlotCount;
    }
}

void UdpService::close_slot(size_t slot)
{
    if (slot >= kUdpSlotCount) {
        return;
    }
    if (packet_slot_ == slot) {
        release_packet();
    }
    discard_received(slot);
    if (slot_open(slot) && backend_.close != nullptr) {
        (void)backend_.close(backend_.context, slots_[slot].handle);
    }
    slots_[slot] = {};
}

void UdpService::release_packet()
{
    packet_buffer_ = nullptr;
    packet_slot_ = kUdpSlotCount;
    packet_size_ = 0U;
    packet_port_ = 0U;
}

void UdpService::close_all()
{
    for (size_t slot = 0U; slot < kUdpSlotCount; ++slot) {
        close_slot(slot);
    }
}

bool UdpService::start_packet(
    size_t slot,
    const uint8_t destination[4],
    uint16_t port)
{
    if (!slot_open(slot) || destination == nullptr || port == 0U ||
        (packet_slot_ != kUdpSlotCount && packet_slot_ != slot)) {
        return false;
    }
    if (packet_buffer_ == nullptr) {
        packet_buffer_ = packet_storage_;
    }
    packet_slot_ = slot;
    packet_size_ = 0U;
    packet_port_ = port;
    memcpy(packet_destination_, destination, sizeof(packet_destination_));
    return true;
}

at::CommandResult UdpService::handle_begin(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write ||
        (command.argument_count != 1U && command.argument_count != 2U) ||
        backend_.open == nullptr) {
        return at::CommandResult::Error;
    }
    unsigned long port = 0U;
    if (!parse_uint(command.arguments[0], 65535U, port) || port == 0U) {
        return at::CommandResult::Error;
    }
    uint8_t local_address[4] = {};
    const uint8_t *local_address_pointer = nullptr;
    if (command.argument_count == 2U) {
        if (!parse_ipv4(command.arguments[1], local_address)) {
            return at::CommandResult::Error;
        }
        local_address_pointer = local_address;
    }

    for (size_t slot = 0U; slot < kUdpSlotCount; ++slot) {
        if (slot_open(slot)) {
            continue;
        }
        int handle = -1;
        if (!backend_.open(
                backend_.context,
                static_cast<uint16_t>(port),
                local_address_pointer,
                &handle) ||
            handle < 0) {
            return at::CommandResult::Error;
        }
        slots_[slot].handle = handle;
        write_prefix(transport, command);
        write_u32(transport, slot);
        (void)transport.write_cstr("\r\n");
        return at::CommandResult::Ok;
    }
    return at::CommandResult::Error;
}

at::CommandResult UdpService::handle_begin_multicast(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 2U ||
        backend_.open == nullptr ||
        backend_.join_multicast == nullptr) {
        return at::CommandResult::Error;
    }
    unsigned long port = 0U;
    uint8_t group[4] = {};
    if (!parse_uint(command.arguments[0], 65535U, port) ||
        port == 0U ||
        !parse_ipv4(command.arguments[1], group) ||
        group[0] < 224U ||
        group[0] > 239U) {
        return at::CommandResult::Error;
    }
    for (size_t slot = 0U; slot < kUdpSlotCount; ++slot) {
        if (slot_open(slot)) {
            continue;
        }
        int handle = -1;
        if (!backend_.open(
                backend_.context,
                static_cast<uint16_t>(port),
                nullptr,
                &handle) ||
            handle < 0 ||
            !backend_.join_multicast(backend_.context, handle, group)) {
            if (handle >= 0 && backend_.close != nullptr) {
                (void)backend_.close(backend_.context, handle);
            }
            return at::CommandResult::Error;
        }
        slots_[slot].handle = handle;
        slots_[slot].multicast = true;
        slots_[slot].multicast_port = static_cast<uint16_t>(port);
        memcpy(slots_[slot].multicast_address, group, sizeof(group));
        write_prefix(transport, command);
        write_u32(transport, slot);
        (void)transport.write_cstr("\r\n");
        return at::CommandResult::Ok;
    }
    return at::CommandResult::Error;
}

at::CommandResult UdpService::handle_begin_packet(
    const at::Command &command,
    at::AtTransport &transport)
{
    size_t slot = 0U;
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        !parse_slot(command, 0U, slot) ||
        !slot_open(slot) ||
        !slots_[slot].remote_valid ||
        !start_packet(slot, slots_[slot].remote_address, slots_[slot].remote_port)) {
        return at::CommandResult::Error;
    }
    (void)transport.write_response(command, "");
    return at::CommandResult::Ok;
}

at::CommandResult UdpService::handle_begin_packet_multicast(
    const at::Command &command,
    at::AtTransport &transport)
{
    size_t slot = 0U;
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        !parse_slot(command, 0U, slot) ||
        !slot_open(slot) ||
        !slots_[slot].multicast ||
        !start_packet(
            slot,
            slots_[slot].multicast_address,
            slots_[slot].multicast_port)) {
        return at::CommandResult::Error;
    }
    (void)transport.write_response(command, "");
    return at::CommandResult::Ok;
}

at::CommandResult UdpService::handle_begin_packet_ip(
    const at::Command &command,
    at::AtTransport &transport)
{
    size_t slot = 0U;
    unsigned long port = 0U;
    uint8_t address[4] = {};
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 3U ||
        !parse_slot(command, 0U, slot) ||
        !parse_uint(command.arguments[1], 65535U, port) ||
        port == 0U ||
        !parse_ipv4(command.arguments[2], address) ||
        !start_packet(slot, address, static_cast<uint16_t>(port))) {
        return at::CommandResult::Error;
    }
    (void)transport.write_response(command, "");
    return at::CommandResult::Ok;
}

at::CommandResult UdpService::handle_begin_packet_name(
    const at::Command &command,
    at::AtTransport &transport)
{
    size_t slot = 0U;
    unsigned long port = 0U;
    uint8_t address[4] = {};
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 3U ||
        !parse_slot(command, 0U, slot) ||
        !parse_uint(command.arguments[1], 65535U, port) ||
        port == 0U ||
        command.arguments[2][0] == '\0' ||
        backend_.resolve_hostname == nullptr) {
        return at::CommandResult::Error;
    }
    NetworkScanWindow scan_window(scan_pause_, scan_resume_, scan_control_context_);
    if (!backend_.resolve_hostname(
            backend_.context,
            command.arguments[2],
            address) ||
        !start_packet(slot, address, static_cast<uint16_t>(port))) {
        return at::CommandResult::Error;
    }
    (void)transport.write_response(command, "");
    return at::CommandResult::Ok;
}

at::CommandResult UdpService::handle_write(
    const at::Command &command,
    at::AtTransport &transport)
{
    size_t slot = 0U;
    unsigned long size = 0U;
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 2U ||
        !parse_slot(command, 0U, slot) ||
        !slot_open(slot) ||
        packet_slot_ != slot ||
        !parse_uint(command.arguments[1], at::kTransportRawPayloadCapacity, size) ||
        size == 0U ||
        size > at::kTransportRawPayloadCapacity - packet_size_) {
        return at::CommandResult::Error;
    }
    if (!transport.request_raw_payload(
            static_cast<size_t>(size),
            &UdpService::append_payload,
            this)) {
        return at::CommandResult::Error;
    }
    return at::CommandResult::Deferred;
}

at::CommandResult UdpService::append_payload(
    void *context,
    const uint8_t *data,
    size_t size)
{
    UdpService *service = static_cast<UdpService *>(context);
    if (service->packet_slot_ == kUdpSlotCount ||
        service->packet_buffer_ == nullptr ||
        data == nullptr ||
        size == 0U ||
        size > at::kTransportRawPayloadCapacity - service->packet_size_) {
        return at::CommandResult::Error;
    }
    memcpy(service->packet_buffer_ + service->packet_size_, data, size);
    service->packet_size_ += size;
    return at::CommandResult::Ok;
}

at::CommandResult UdpService::handle_end_packet(
    const at::Command &command,
    at::AtTransport &transport)
{
    size_t slot = 0U;
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        !parse_slot(command, 0U, slot) ||
        !slot_open(slot) ||
        packet_slot_ != slot ||
        backend_.send == nullptr) {
        return at::CommandResult::Error;
    }
    size_t sent = 0U;
    NetworkScanWindow scan_window(scan_pause_, scan_resume_, scan_control_context_);
    const bool sent_all = backend_.send(
        backend_.context,
        slots_[slot].handle,
        packet_destination_,
        packet_port_,
        packet_buffer_,
        packet_size_,
        &sent) &&
        sent == packet_size_;
    release_packet();
    if (!sent_all) {
        return at::CommandResult::Error;
    }
    (void)transport.write_response(command, "");
    return at::CommandResult::Ok;
}

at::CommandResult UdpService::handle_parse(
    const at::Command &command,
    at::AtTransport &transport)
{
    size_t slot = 0U;
    const bool slot_valid = parse_slot(command, 0U, slot);
    const bool is_open = slot_valid && slot_open(slot);
    const bool backend_present = backend_.receive != nullptr;
#if defined(ARDUINO)
    udp_socket_diagnostic_note_parse(
        static_cast<uint8_t>(command.mode),
        static_cast<uint8_t>(command.argument_count),
        static_cast<uint8_t>(slot),
        slot_valid,
        is_open,
        backend_present);
#endif
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        !slot_valid ||
        !is_open ||
        !backend_present) {
        return at::CommandResult::Error;
    }

    if (slots_[slot].received != nullptr &&
        slots_[slot].received_position < slots_[slot].received_size) {
        write_prefix(transport, command);
        write_u32(transport, slots_[slot].received_size);
        (void)transport.write_cstr("\r\n");
        return at::CommandResult::Ok;
    }
    if (received_storage_slot_ != kUdpSlotCount &&
        received_storage_slot_ != slot) {
        write_prefix(transport, command);
        write_u32(transport, 0U);
        (void)transport.write_cstr("\r\n");
        return at::CommandResult::Ok;
    }
    discard_received(slot);

    uint8_t *received = received_storage_;
    size_t received_size = 0U;
    uint8_t source_address[4] = {};
    uint16_t source_port = 0U;
    NetworkScanWindow scan_window(scan_pause_, scan_resume_, scan_control_context_);
    const bool received_packet = backend_.receive(
        backend_.context,
        slots_[slot].handle,
        received,
        at::kTransportRawPayloadCapacity,
        &received_size,
        source_address,
        &source_port);
    if (!received_packet || received_size > at::kTransportRawPayloadCapacity) {
        return at::CommandResult::Error;
    }
    if (received_size != 0U) {
        slots_[slot].received = received;
        slots_[slot].received_size = received_size;
        slots_[slot].received_position = 0U;
        received_storage_slot_ = slot;
        memcpy(slots_[slot].remote_address, source_address, sizeof(source_address));
        slots_[slot].remote_port = source_port;
        slots_[slot].remote_valid = true;
    } else {
        discard_received(slot);
    }
    write_prefix(transport, command);
    write_u32(transport, received_size);
    (void)transport.write_cstr("\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult UdpService::handle_available(
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
    const size_t available = slots_[slot].received_size - slots_[slot].received_position;
    write_prefix(transport, command);
    write_u32(transport, available);
    (void)transport.write_cstr("\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult UdpService::handle_read(
    const at::Command &command,
    at::AtTransport &transport)
{
    size_t slot = 0U;
    unsigned long requested = 0U;
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 2U ||
        !parse_slot(command, 0U, slot) ||
        !slot_open(slot) ||
        !parse_uint(command.arguments[1], at::kTransportRawPayloadCapacity, requested) ||
        requested == 0U) {
        return at::CommandResult::Error;
    }
    const size_t available = slots_[slot].received_size - slots_[slot].received_position;
    const size_t count = static_cast<size_t>(requested) < available
        ? static_cast<size_t>(requested)
        : available;
    write_prefix(transport, command);
    write_u32(transport, count);
    (void)transport.write_cstr("|");
    if (count != 0U) {
        (void)transport.write_borrowed_data(
            slots_[slot].received + slots_[slot].received_position,
            count);
        slots_[slot].received_position += count;
        if (slots_[slot].received_position == slots_[slot].received_size) {
            discard_received(slot);
        }
    }
    return at::CommandResult::Ok;
}

at::CommandResult UdpService::handle_peek(
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
    const int value = slots_[slot].received_position < slots_[slot].received_size
        ? slots_[slot].received[slots_[slot].received_position]
        : -1;
    write_prefix(transport, command);
    write_i32(transport, value);
    (void)transport.write_cstr("\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult UdpService::handle_flush(
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
    discard_received(slot);
    (void)transport.write_response(command, "");
    return at::CommandResult::Ok;
}

at::CommandResult UdpService::handle_remote_ip(
    const at::Command &command,
    at::AtTransport &transport)
{
    size_t slot = 0U;
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        !parse_slot(command, 0U, slot) ||
        !slot_open(slot) ||
        !slots_[slot].remote_valid) {
        return at::CommandResult::Error;
    }
    write_prefix(transport, command);
    write_ip(transport, slots_[slot].remote_address);
    (void)transport.write_cstr("\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult UdpService::handle_remote_port(
    const at::Command &command,
    at::AtTransport &transport)
{
    size_t slot = 0U;
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        !parse_slot(command, 0U, slot) ||
        !slot_open(slot) ||
        !slots_[slot].remote_valid) {
        return at::CommandResult::Error;
    }
    write_prefix(transport, command);
    write_u32(transport, slots_[slot].remote_port);
    (void)transport.write_cstr("\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult UdpService::handle_debug(
    const at::Command &command,
    at::AtTransport &transport)
{
    size_t slot = 0U;
    UdpDiagnostics diagnostics = {};
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        !parse_slot(command, 0U, slot) ||
        !slot_open(slot) ||
        backend_.diagnostics == nullptr ||
        !backend_.diagnostics(
            backend_.context,
            slots_[slot].handle,
            &diagnostics)) {
        return at::CommandResult::Error;
    }

    /*
     * This is an intentionally numeric, test-only observability command. It
     * avoids SSIDs, credentials, MAC addresses, and application payloads.
     */
    write_prefix(transport, command);
    write_u32(transport, diagnostics.flags);
    (void)transport.write_cstr(",");
    write_i32(transport, diagnostics.last_send_result);
    (void)transport.write_cstr(",");
    write_i32(transport, diagnostics.last_errno);
    (void)transport.write_cstr(",");
    write_hex_ip(transport, diagnostics.sta_address);
    (void)transport.write_cstr(",");
    write_hex_ip(transport, diagnostics.sta_gateway);
    (void)transport.write_cstr(",");
    write_hex_ip(transport, diagnostics.default_address);
    (void)transport.write_cstr(",");
    write_hex_ip(transport, diagnostics.local_address);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.local_port);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.send_heap_used_before);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.send_heap_used_after);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.send_heap_errors);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.send_udp_xmit);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.send_fragment_xmit);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.send_fragment_errors);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.fragment_confirmed);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.fragment_acknowledged);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.fragment_discarded);
    // Passive lifetime raw timeout evidence and the current staged datagram.
    const auto raw = transport.raw_payload_timeout_diagnostic();
    const uint32_t staged[] = {static_cast<uint32_t>(packet_slot_),
                               static_cast<uint32_t>(packet_size_),
                               raw.count, raw.expected, raw.received
#if defined(BL616CL_UDP_FRAGMENT_CONTENT_DIAGNOSTIC) && BL616CL_UDP_FRAGMENT_CONTENT_DIAGNOSTIC
                               , diagnostics.fragment_content_active,
                               diagnostics.fragment_content_checked,
                               diagnostics.fragment_header_errors,
                               diagnostics.fragment_payload_errors,
                               diagnostics.fragment_content_mask,
                               diagnostics.fragment_identification,
                               diagnostics.fragment_udp_checksum_errors,
                               diagnostics.fragment_udp_checksum_residual
#endif
                               };
    for (uint32_t value : staged) {
        (void)transport.write_cstr(",");
        write_u32(transport, value);
    }
    (void)transport.write_cstr("\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult UdpService::handle_stop(
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

} // namespace bridge
