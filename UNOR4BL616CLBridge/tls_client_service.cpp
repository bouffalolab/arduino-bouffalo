#include "tls_client_service.h"
#include "network_scan_window.h"
#include "tls_window_timing.h"
#if BL616CL_TLS_WINDOW_TIMING
#include <Arduino.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace bridge {

namespace {

constexpr char kSslBeginClient[] = "+SSLBEGINCLIENT";
constexpr char kSetCaRoot[] = "+SETCAROOT";
constexpr char kSetEccSlot[] = "+SETECCSLOT";
constexpr char kSslClientState[] = "+SSLCLIENTSTATE";
constexpr char kSslClientConnectName[] = "+SSLCLIENTCONNECTNAME";
constexpr char kSslClientConnectIp[] = "+SSLCLIENTCONNECTIP";
constexpr char kSslClientConnect[] = "+SSLCLIENTCONNECT";
constexpr char kSslClientSend[] = "+SSLCLIENTSEND";
constexpr char kSslClientReceive[] = "+SSLCLIENTRECEIVE";
constexpr char kSslClientClose[] = "+SSLCLIENTCLOSE";
constexpr char kSslIpClient[] = "+SSLIPCLIENT";
constexpr char kSslClientConnected[] = "+SSLCLIENTCONNECTED";
constexpr char kSslAvailable[] = "+SSLAVAILABLE";
constexpr char kSslClientStatus[] = "+SSLCLIENTSTATUS";
constexpr char kSslClientFlush[] = "+SSLCLIENTFLUSH";
constexpr char kSslRemoteIp[] = "+SSLREMOTEIP";
constexpr char kSslRemotePort[] = "+SSLREMOTEPORT";
constexpr char kSslPeek[] = "+SSLPEEK";
constexpr char kSslClientDiagnostic[] = "+SSLCLIENTDIAG";
constexpr char kSslClientVerifyFlags[] = "+SSLCLIENTVERIFYFLAGS";
constexpr char kSslClientEndpoint[] = "+SSLCLIENTENDPOINT";
constexpr char kTlsWindowDiagnostic[] = "+TLSWINDOWDIAG";
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

void write_endpoint_ip(
    at::AtTransport &transport,
    const uint8_t address[4],
    bool valid)
{
    if (valid) {
        write_ip(transport, address);
    } else {
        (void)transport.write_cstr("0.0.0.0");
    }
}

void write_prefix(at::AtTransport &transport, const at::Command &command)
{
    (void)transport.write_cstr(command.name);
    (void)transport.write_cstr(": ");
}

void clear_sensitive(uint8_t *data, size_t size)
{
    if (data == nullptr) {
        return;
    }
    volatile uint8_t *cursor = data;
    for (size_t index = 0U; index < size; ++index) {
        cursor[index] = 0U;
    }
}

} // namespace

TlsClientService::TlsClientService(
    TlsClientBackend backend,
    TlsPrivateKeyResolver private_key_resolver,
    void *private_key_resolver_context)
    : backend_(backend),
      private_key_resolver_(private_key_resolver),
      private_key_resolver_context_(private_key_resolver_context)
{
}

TlsClientService::~TlsClientService()
{
    close_all();
}

void TlsClientService::set_scan_control(
    ScanPause pause, ScanResume resume, void *context)
{
    scan_pause_ = pause;
    scan_resume_ = resume;
    scan_control_context_ = context;
}

bool TlsClientService::handles_command(const at::Command &command) const
{
    return is_name(command, kSslBeginClient) ||
        is_name(command, kSetCaRoot) ||
        is_name(command, kSetEccSlot) ||
        is_name(command, kSslClientState) ||
        is_name(command, kSslClientConnectName) ||
        is_name(command, kSslClientConnectIp) ||
        is_name(command, kSslClientConnect) ||
        is_name(command, kSslClientSend) ||
        is_name(command, kSslClientReceive) ||
        is_name(command, kSslClientClose) ||
        is_name(command, kSslIpClient) ||
        is_name(command, kSslClientConnected) ||
        is_name(command, kSslAvailable) ||
        is_name(command, kSslClientStatus) ||
        is_name(command, kSslClientFlush) ||
        is_name(command, kSslRemoteIp) ||
        is_name(command, kSslRemotePort) ||
        is_name(command, kSslPeek) ||
        is_name(command, kSslClientDiagnostic) ||
        is_name(command, kSslClientVerifyFlags) ||
        is_name(command, kSslClientEndpoint) || is_name(command, kTlsWindowDiagnostic);
}

void TlsClientService::close_all()
{
    for (size_t slot = 0U; slot < kTlsClientSlotCount; ++slot) {
        close_slot(slot);
    }
}

bool TlsClientService::parse_uint(
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

bool TlsClientService::parse_slot(
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
            kTlsClientSocketBase + kTlsClientSlotCount - 1U,
            parsed) ||
        parsed < kTlsClientSocketBase) {
        return false;
    }
    slot = static_cast<size_t>(parsed - kTlsClientSocketBase);
    return true;
}

bool TlsClientService::parse_ipv4(const char *text, uint8_t address[4]) const
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

bool TlsClientService::slot_open(size_t slot) const
{
    return slot < kTlsClientSlotCount && slots_[slot].handle >= 0;
}

void TlsClientService::close_slot(size_t slot)
{
    if (slot >= kTlsClientSlotCount) {
        return;
    }
    if (slot_open(slot) && backend_.close != nullptr) {
        (void)backend_.close(backend_.context, slots_[slot].handle);
    }
    release_ca(slot);
    clear_client_credentials(slot);
    slots_[slot] = {};
    if (pending_ca_slot_ == slot) {
        pending_ca_slot_ = kTlsClientSlotCount;
    }
    if (pending_send_slot_ == slot) {
        pending_send_slot_ = kTlsClientSlotCount;
    }
    if (pending_ecc_slot_ == slot) {
        pending_ecc_slot_ = kTlsClientSlotCount;
        pending_ecc_key_slot_[0] = '\0';
    }
}

void TlsClientService::release_ca(size_t slot)
{
    uint8_t *certificate = slots_[slot].ca_certificate;
    slots_[slot].ca_certificate = nullptr;
    slots_[slot].ca_certificate_size = 0U;
    if (certificate == nullptr) return;
    for (const auto &other : slots_) {
        if (other.ca_certificate == certificate) return;
    }
    free(certificate);
}

bool TlsClientService::replace_ca(size_t slot, const uint8_t *data, size_t size)
{
    if (slot >= kTlsClientSlotCount ||
        data == nullptr ||
        size == 0U ||
        size > at::kTransportRawPayloadCapacity) {
        return false;
    }
    // Raw trust anchors are immutable. Share identical bytes across slots,
    // while each backend retains its own parsed chain and verification state.
    for (size_t other = 0U; other < kTlsClientSlotCount; ++other) {
        if (slots_[other].ca_certificate != nullptr &&
            slots_[other].ca_certificate_size == size + 1U &&
            memcmp(slots_[other].ca_certificate, data, size) == 0) {
            if (other == slot) return true;
            uint8_t *shared = slots_[other].ca_certificate;
            release_ca(slot);
            slots_[slot].ca_certificate = shared;
            slots_[slot].ca_certificate_size = size + 1U;
            return true;
        }
    }
    uint8_t *copy = static_cast<uint8_t *>(malloc(size + 1U));
    if (copy == nullptr) return false;
    memcpy(copy, data, size);
    copy[size] = '\0';
    release_ca(slot);
    slots_[slot].ca_certificate = copy;
    slots_[slot].ca_certificate_size = size + 1U;
    return true;
}

void TlsClientService::clear_client_credentials(size_t slot)
{
    if (slot >= kTlsClientSlotCount) {
        return;
    }
    free(slots_[slot].client_certificate);
    slots_[slot].client_certificate = nullptr;
    slots_[slot].client_certificate_size = 0U;
    memset(
        slots_[slot].client_private_key_slot,
        0,
        sizeof(slots_[slot].client_private_key_slot));
}

bool TlsClientService::replace_client_credentials(
    size_t slot,
    const uint8_t *certificate,
    size_t certificate_size,
    const char *key_slot)
{
    if (slot >= kTlsClientSlotCount ||
        certificate == nullptr ||
        certificate_size == 0U ||
        certificate_size > at::kTransportRawPayloadCapacity ||
        key_slot == nullptr ||
        key_slot[0] == '\0' ||
        private_key_resolver_ == nullptr) {
        return false;
    }

    uint8_t private_key[kTlsClientPrivateKeyDerCapacity] = {};
    size_t private_key_size = 0U;
    const bool private_key_resolved = private_key_resolver_(
            private_key_resolver_context_,
            key_slot,
            private_key,
            sizeof(private_key),
            &private_key_size);
    if (!private_key_resolved ||
        private_key_size == 0U ||
        private_key_size > sizeof(private_key)) {
        clear_sensitive(private_key, sizeof(private_key));
        return false;
    }

    uint8_t *certificate_copy =
        static_cast<uint8_t *>(malloc(certificate_size));
    if (certificate_copy == nullptr) {
        free(certificate_copy);
        clear_sensitive(private_key, sizeof(private_key));
        return false;
    }
    memcpy(certificate_copy, certificate, certificate_size);
    clear_sensitive(private_key, sizeof(private_key));

    clear_client_credentials(slot);
    slots_[slot].client_certificate = certificate_copy;
    slots_[slot].client_certificate_size = certificate_size;
    strncpy(
        slots_[slot].client_private_key_slot,
        key_slot,
        sizeof(slots_[slot].client_private_key_slot) - 1U);
    slots_[slot].client_private_key_slot[
        sizeof(slots_[slot].client_private_key_slot) - 1U] = '\0';
    return true;
}

at::CommandResult TlsClientService::handle_begin(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Run ||
        backend_.create == nullptr) {
        return at::CommandResult::Error;
    }
    for (size_t slot = 0U; slot < kTlsClientSlotCount; ++slot) {
        if (slot_open(slot)) {
            continue;
        }
        int handle = -1;
        if (!backend_.create(backend_.context, &handle) || handle < 0) {
            return at::CommandResult::Error;
        }
        slots_[slot].handle = handle;
        write_prefix(transport, command);
        write_u32(transport, kTlsClientSocketBase + slot);
        (void)transport.write_cstr("\r\n");
        return at::CommandResult::Ok;
    }
    return at::CommandResult::Error;
}

at::CommandResult TlsClientService::handle_set_ca(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write ||
        (command.argument_count != 1U && command.argument_count != 2U)) {
        return at::CommandResult::Error;
    }
    size_t slot = 0U;
    if (!parse_slot(command, 0U, slot) || !slot_open(slot)) {
        return at::CommandResult::Error;
    }
    if (command.argument_count == 1U) {
        /*
         * The ESP32 implementation loads its generated CA bundle here.
         * Until a BL616CL bundle attachment is added, accepting this form
         * would silently turn certificate verification off.
         */
        return at::CommandResult::Error;
    }

    unsigned long size = 0U;
    if (!parse_uint(
            command.arguments[1],
            at::kTransportRawPayloadCapacity,
            size) ||
        size == 0U) {
        return at::CommandResult::Error;
    }
    pending_ca_slot_ = slot;
    if (!transport.request_raw_payload(
            static_cast<size_t>(size),
            &TlsClientService::set_ca_payload,
            this)) {
        pending_ca_slot_ = kTlsClientSlotCount;
        return at::CommandResult::Error;
    }
    return at::CommandResult::Deferred;
}

at::CommandResult TlsClientService::set_ca_payload(
    void *context,
    const uint8_t *data,
    size_t size)
{
    TlsClientService *service = static_cast<TlsClientService *>(context);
    const size_t slot = service->pending_ca_slot_;
    service->pending_ca_slot_ = kTlsClientSlotCount;
    if (!service->slot_open(slot) ||
        !service->replace_ca(slot, data, size)) {
        return at::CommandResult::Error;
    }
    return at::CommandResult::Ok;
}

at::CommandResult TlsClientService::handle_set_ecc_slot(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 3U) {
        return at::CommandResult::Error;
    }
    size_t slot = 0U;
    unsigned long key_slot = 0U;
    unsigned long certificate_size = 0U;
    if (!parse_slot(command, 0U, slot) ||
        !slot_open(slot) ||
        !parse_uint(command.arguments[1], 2147483647UL, key_slot) ||
        !parse_uint(
            command.arguments[2],
            at::kTransportRawPayloadCapacity,
            certificate_size) ||
        certificate_size == 0U ||
        private_key_resolver_ == nullptr) {
        return at::CommandResult::Error;
    }
    const int written = snprintf(
        pending_ecc_key_slot_,
        sizeof(pending_ecc_key_slot_),
        "%lu",
        key_slot);
    if (written <= 0 ||
        static_cast<size_t>(written) >= sizeof(pending_ecc_key_slot_)) {
        pending_ecc_key_slot_[0] = '\0';
        return at::CommandResult::Error;
    }
    pending_ecc_slot_ = slot;
    if (!transport.request_raw_payload(
            static_cast<size_t>(certificate_size),
            &TlsClientService::set_ecc_slot_payload,
            this)) {
        pending_ecc_slot_ = kTlsClientSlotCount;
        pending_ecc_key_slot_[0] = '\0';
        return at::CommandResult::Error;
    }
    return at::CommandResult::Deferred;
}

at::CommandResult TlsClientService::set_ecc_slot_payload(
    void *context,
    const uint8_t *data,
    size_t size)
{
    TlsClientService *service = static_cast<TlsClientService *>(context);
    const size_t slot = service->pending_ecc_slot_;
    service->pending_ecc_slot_ = kTlsClientSlotCount;
    const bool replaced =
        service->slot_open(slot) &&
        service->replace_client_credentials(
            slot,
            data,
            size,
            service->pending_ecc_key_slot_);
    service->pending_ecc_key_slot_[0] = '\0';
    return replaced ? at::CommandResult::Ok : at::CommandResult::Error;
}

at::CommandResult TlsClientService::handle_connect(
    const at::Command &command,
    at::AtTransport &transport)
{
    const bool name_connect = is_name(command, kSslClientConnectName);
    const bool ip_connect = is_name(command, kSslClientConnectIp);
    const bool generic_connect = is_name(command, kSslClientConnect);
    if (command.mode != at::CommandMode::Write ||
        (!name_connect && !ip_connect && !generic_connect) ||
        command.argument_count < 3U ||
        (generic_connect
            ? command.argument_count > 4U
            : command.argument_count != 3U)) {
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
    uint32_t timeout_ms = kDefaultConnectTimeoutMs;
    if (generic_connect && command.argument_count == 4U) {
        unsigned long parsed = 0U;
        if (!parse_uint(command.arguments[3], 600000U, parsed) || parsed == 0U) {
            return at::CommandResult::Error;
        }
        timeout_ms = static_cast<uint32_t>(parsed);
    }
    if (slots_[slot].ca_certificate == nullptr ||
        slots_[slot].ca_certificate_size == 0U) {
        return at::CommandResult::Error;
    }

    const uint8_t *ca_certificate = slots_[slot].ca_certificate;
    const size_t ca_certificate_size = slots_[slot].ca_certificate_size;
    const uint8_t *client_certificate = slots_[slot].client_certificate;
    const size_t client_certificate_size =
        slots_[slot].client_certificate_size;
    uint8_t address[4] = {};
    if (name_connect || generic_connect) {
        if (command.arguments[1][0] == '\0' ||
            backend_.connect_hostname == nullptr) {
            return at::CommandResult::Error;
        }
    } else if (!parse_ipv4(command.arguments[1], address) ||
               backend_.connect_ip == nullptr) {
        return at::CommandResult::Error;
    }

    uint8_t client_private_key[kTlsClientPrivateKeyDerCapacity] = {};
    size_t client_private_key_size = 0U;
    if (client_certificate != nullptr) {
        const bool private_key_resolved =
            slots_[slot].client_private_key_slot[0] != '\0' &&
            private_key_resolver_(
                private_key_resolver_context_,
                slots_[slot].client_private_key_slot,
                client_private_key,
                sizeof(client_private_key),
                &client_private_key_size);
        if (!private_key_resolved ||
            client_private_key_size == 0U ||
            client_private_key_size > sizeof(client_private_key)) {
            clear_sensitive(
                client_private_key,
                sizeof(client_private_key));
            return at::CommandResult::Error;
        }
    }
    const uint8_t *client_private_key_data =
        client_certificate == nullptr ? nullptr : client_private_key;
    bool connected = false;
    NetworkScanWindow scan_window(scan_pause_, scan_resume_, scan_control_context_);
    if (name_connect || generic_connect) {
        connected = backend_.connect_hostname(
            backend_.context,
            slots_[slot].handle,
            command.arguments[1],
            static_cast<uint16_t>(port),
            timeout_ms,
            ca_certificate,
            ca_certificate_size,
            client_certificate,
            client_certificate_size,
            client_private_key_data,
            client_private_key_size);
    } else {
        connected = backend_.connect_ip(
            backend_.context,
            slots_[slot].handle,
            address,
            static_cast<uint16_t>(port),
            timeout_ms,
            ca_certificate,
            ca_certificate_size,
            client_certificate,
            client_certificate_size,
            client_private_key_data,
            client_private_key_size);
    }
    clear_sensitive(client_private_key, sizeof(client_private_key));
    if (!connected) {
        return at::CommandResult::Error;
    }
    (void)transport.write_response(command, "");
    return at::CommandResult::Ok;
}

at::CommandResult TlsClientService::handle_send(
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
            &TlsClientService::send_payload,
            this)) {
        pending_send_slot_ = kTlsClientSlotCount;
        return at::CommandResult::Error;
    }
    return at::CommandResult::Deferred;
}

at::CommandResult TlsClientService::send_payload(
    void *context,
    const uint8_t *data,
    size_t size)
{
    TlsClientService *service = static_cast<TlsClientService *>(context);
    const size_t slot = service->pending_send_slot_;
    service->pending_send_slot_ = kTlsClientSlotCount;
    if (!service->slot_open(slot) || service->backend_.send == nullptr) {
        return at::CommandResult::Error;
    }
    NetworkScanWindow scan_window(service->scan_pause_, service->scan_resume_,
                           service->scan_control_context_);
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

at::CommandResult TlsClientService::handle_receive(
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
    (void)transport.write_cstr("\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult TlsClientService::handle_close(
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

at::CommandResult TlsClientService::handle_state(
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
    (void)transport.write_cstr("\r\n\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult TlsClientService::handle_simple_slot_command(
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
    if (is_name(command, kSslAvailable)) {
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
    if (is_name(command, kSslPeek)) {
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
    if (is_name(command, kSslClientConnected)) {
        bool connected = false;
        if (backend_.connected == nullptr ||
            !backend_.connected(backend_.context, slots_[slot].handle, &connected)) {
            return at::CommandResult::Error;
        }
        (void)transport.write_response(command, connected ? "1" : "0");
        return at::CommandResult::Ok;
    }
    if (is_name(command, kSslClientStatus)) {
        write_prefix(transport, command);
        (void)transport.write_cstr("\r\n");
        return at::CommandResult::Ok;
    }
    if (is_name(command, kSslClientFlush)) {
        if (backend_.flush == nullptr ||
            !backend_.flush(backend_.context, slots_[slot].handle)) {
            return at::CommandResult::Error;
        }
        write_prefix(transport, command);
        (void)transport.write_cstr("\r\n");
        return at::CommandResult::Ok;
    }
    if (is_name(command, kSslIpClient)) {
        bool connected = false;
        if (backend_.connected == nullptr ||
            !backend_.connected(backend_.context, slots_[slot].handle, &connected)) {
            return at::CommandResult::Error;
        }
        if (!connected) {
            return at::CommandResult::Ok;
        }
        uint8_t address[4] = {};
        if (backend_.get_local_address == nullptr ||
            !backend_.get_local_address(
                backend_.context, slots_[slot].handle, address)) {
            return at::CommandResult::Error;
        }
        write_prefix(transport, command);
        write_ip(transport, address);
        (void)transport.write_cstr("\r\n\r\n");
        return at::CommandResult::Ok;
    }
    if (is_name(command, kSslRemoteIp) ||
        is_name(command, kSslRemotePort)) {
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
        if (is_name(command, kSslRemoteIp)) {
            write_ip(transport, address);
        } else {
            write_u32(transport, remote_port);
        }
        (void)transport.write_cstr("\r\n");
        return at::CommandResult::Ok;
    }
    return at::CommandResult::Error;
}

at::CommandResult TlsClientService::handle_diagnostic(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        backend_.get_diagnostic == nullptr) {
        return at::CommandResult::Error;
    }
    size_t slot = 0U;
    if (!parse_slot(command, 0U, slot) || !slot_open(slot)) {
        return at::CommandResult::Error;
    }
    uint8_t phase = 0U;
    int32_t error = 0;
    if (!backend_.get_diagnostic(
            backend_.context,
            slots_[slot].handle,
            &phase,
            &error)) {
        return at::CommandResult::Error;
    }
    write_prefix(transport, command);
    write_u32(transport, phase);
    (void)transport.write_cstr(",");
    write_i32(transport, error);
    if (backend_.get_heap_diagnostic != nullptr) {
        TlsHeapDiagnostic heap = {};
        if (!backend_.get_heap_diagnostic(
                backend_.context,
                slots_[slot].handle,
                &heap)) {
            return at::CommandResult::Error;
        }
        (void)transport.write_cstr(",");
        write_u32(transport, heap.before_handshake_free_bytes);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.failure_free_bytes);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.after_cleanup_free_bytes);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.connection_timeout_ms);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.handshake_elapsed_ms);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.before_setup_ocram_free_bytes);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.before_setup_wram_free_bytes);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.failure_ocram_free_bytes);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.failure_wram_free_bytes);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.after_cleanup_ocram_free_bytes);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.after_cleanup_wram_free_bytes);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.observed_ocram_free_bytes);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.observed_wram_free_bytes);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.before_setup_wram_max_block_bytes);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.failure_wram_max_block_bytes);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.after_cleanup_wram_max_block_bytes);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.observed_wram_max_block_bytes);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.failure_tcp_pcb_used);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.failure_tcp_seg_used);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.failure_netconn_used);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.observed_tcp_pcb_used);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.observed_tcp_seg_used);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.observed_netconn_used);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.tcp_seg_allocation_errors);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.failure_lwip_heap_used);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.observed_lwip_heap_used);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.lwip_heap_allocation_errors);
        for (uint32_t count : heap.tcp_state_count) {
            (void)transport.write_cstr(",");
            write_u32(transport, count);
        }
        (void)transport.write_cstr(",");
        write_u32(transport, heap.tcp_unacked_pbuf_bytes);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.tcp_unsent_pbuf_bytes);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.tcp_ooseq_pbuf_bytes);
        (void)transport.write_cstr(",");
        write_u32(transport, heap.tcp_max_retransmissions);
#if BL616CL_TLS_FLOW_DIAGNOSTIC
        for (uint32_t value : heap.flow.values) {
            (void)transport.write_cstr(",");
            write_u32(transport, value);
        }
#endif
    }
    (void)transport.write_cstr("\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult TlsClientService::handle_verify_flags(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        backend_.get_verify_flags == nullptr) {
        return at::CommandResult::Error;
    }
    size_t slot = 0U;
    if (!parse_slot(command, 0U, slot) || !slot_open(slot)) {
        return at::CommandResult::Error;
    }
    uint32_t flags = 0U;
    if (!backend_.get_verify_flags(
            backend_.context,
            slots_[slot].handle,
            &flags)) {
        return at::CommandResult::Error;
    }
    write_prefix(transport, command);
    write_u32(transport, flags);
    (void)transport.write_cstr("\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult TlsClientService::handle_endpoint(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        backend_.get_endpoint_diagnostic == nullptr) {
        return at::CommandResult::Error;
    }
    size_t slot = 0U;
    if (!parse_slot(command, 0U, slot) || !slot_open(slot)) {
        return at::CommandResult::Error;
    }
    TlsEndpointDiagnostic endpoint = {};
    if (!backend_.get_endpoint_diagnostic(
            backend_.context,
            slots_[slot].handle,
            &endpoint)) {
        return at::CommandResult::Error;
    }
    write_prefix(transport, command);
    write_endpoint_ip(
        transport,
        endpoint.resolved_address,
        endpoint.resolved_valid);
    (void)transport.write_cstr(",");
    write_endpoint_ip(
        transport,
        endpoint.peer_address,
        endpoint.peer_valid);
    (void)transport.write_cstr(",");
    write_u32(transport, endpoint.peer_port);
    (void)transport.write_cstr("\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult TlsClientService::handle_command(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (is_name(command, kTlsWindowDiagnostic)) {
        if (command.mode != at::CommandMode::Read) return at::CommandResult::Error;
        write_prefix(transport, command);
        (void)transport.write_cstr("1,");
        write_u32(transport, BL616CL_TLS_WINDOW_TIMING);
#if BL616CL_TLS_WINDOW_TIMING
        const auto &counters = tls_window_timing_counters();
        (void)transport.write_cstr(","); write_u32(transport, counters.overflow);
        for (const auto &group : counters.groups) {
            (void)transport.write_cstr(","); write_u32(transport, group.count);
            (void)transport.write_cstr(","); write_u32(transport, group.paused);
            for (uint32_t value : group.sum) { (void)transport.write_cstr(","); write_u32(transport, value); }
            for (uint32_t value : group.maximum) { (void)transport.write_cstr(","); write_u32(transport, value); }
        }
#endif
        (void)transport.write_cstr("\r\n");
        return at::CommandResult::Ok;
    }
    // Raw sends acquire their window only after the complete payload arrives.
    const bool receive_window = is_name(command, kSslClientReceive) ||
        is_name(command, kSslAvailable) || is_name(command, kSslPeek);
#if BL616CL_TLS_CACHED_AVAILABLE
    size_t cached_bytes = 0U;
    size_t cached_slot = 0U;
    const bool cached_available = is_name(command, kSslAvailable) &&
        command.mode == at::CommandMode::Write && command.argument_count == 1U &&
        parse_slot(command, 0U, cached_slot) && slot_open(cached_slot) &&
        backend_.cached_available != nullptr &&
        backend_.cached_available(backend_.context, slots_[cached_slot].handle,
                                  &cached_bytes) && cached_bytes != 0U;
    const bool needs_scan_window = receive_window && !cached_available;
#else
    const bool needs_scan_window = receive_window;
#endif
#if BL616CL_TLS_WINDOW_TIMING
    TlsWindowTiming timing(!receive_window ? -1 : is_name(command, kSslAvailable) ? 0 :
                          is_name(command, kSslPeek) ? 1 : 2,
                          []() -> uint32_t { return micros(); });
#endif
    NetworkScanWindow scan_window(needs_scan_window ? scan_pause_ : nullptr,
                           scan_resume_, scan_control_context_);
#if BL616CL_TLS_WINDOW_TIMING
    timing.pause_finished(scan_window.paused());
    // Destroy before scan_window, then timing: body -> resume -> accumulation.
    TlsWindowTiming::Body timing_body(timing);
#endif
#if BL616CL_TLS_CACHED_AVAILABLE
    if (cached_available) {
        write_prefix(transport, command);
        write_u32(transport, cached_bytes);
        (void)transport.write_cstr("\r\n");
        return at::CommandResult::Ok;
    }
#endif
    if (is_name(command, kSslBeginClient)) {
        return handle_begin(command, transport);
    }
    if (is_name(command, kSetCaRoot)) {
        return handle_set_ca(command, transport);
    }
    if (is_name(command, kSetEccSlot)) {
        return handle_set_ecc_slot(command, transport);
    }
    if (is_name(command, kSslClientConnectName) ||
        is_name(command, kSslClientConnectIp) ||
        is_name(command, kSslClientConnect)) {
        return handle_connect(command, transport);
    }
    if (is_name(command, kSslClientSend)) {
        return handle_send(command, transport);
    }
    if (is_name(command, kSslClientReceive)) {
        return handle_receive(command, transport);
    }
    if (is_name(command, kSslClientClose)) {
        return handle_close(command, transport);
    }
    if (is_name(command, kSslClientState)) {
        return handle_state(command, transport);
    }
    if (is_name(command, kSslClientDiagnostic)) {
        return handle_diagnostic(command, transport);
    }
    if (is_name(command, kSslClientVerifyFlags)) {
        return handle_verify_flags(command, transport);
    }
    if (is_name(command, kSslClientEndpoint)) {
        return handle_endpoint(command, transport);
    }
    return handle_simple_slot_command(command, transport);
}

} // namespace bridge
