#include "udp_runtime_diagnostic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace bridge {

namespace {

constexpr char kUdpRuntime[] = "+UDPRUNTIME";
constexpr uint8_t kRequest[] = "bl616cl-udp-echo";
constexpr uint8_t kExpectedReply[] = "bl616cl-udp-reply";
constexpr uint8_t kSendAttempts = 8U;
constexpr uint32_t kReceiveWindowMs = 2000U;

enum class UdpRuntimeStage : uint8_t {
    Passed = 0U,
    Open = 1U,
    Send = 2U,
    ReceiveTimeout = 3U,
    Reply = 4U,
};

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

void write_result(
    at::AtTransport &transport,
    bool passed,
    UdpRuntimeStage stage,
    uint8_t attempts,
    size_t received,
    const UdpDiagnostics &diagnostics,
    const uint8_t source_address[4],
    uint16_t source_port)
{
    /*
     * Numeric-only output: no Wi-Fi credentials, MAC address, or payload.
     * Fields are pass, stage, sends, received bytes, backend flags,
     * sendto result, errno, response IPv4 and response port, then the
     * direct-probe lwIP, Wi-Fi netif-output, FHOST TX-confirmation, and
     * IPv4/UDP header-validation fields.
     */
    (void)transport.write_cstr("+UDPRUNTIME: ");
    write_u32(transport, passed ? 1U : 0U);
    (void)transport.write_cstr(",");
    write_u32(transport, static_cast<uint8_t>(stage));
    (void)transport.write_cstr(",");
    write_u32(transport, attempts);
    (void)transport.write_cstr(",");
    write_u32(transport, received);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.flags);
    (void)transport.write_cstr(",");
    write_i32(transport, diagnostics.last_send_result);
    (void)transport.write_cstr(",");
    write_i32(transport, diagnostics.last_errno);
    (void)transport.write_cstr(",");
    write_hex_ip(transport, source_address);
    (void)transport.write_cstr(",");
    write_u32(transport, source_port);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.lwip_udp_xmit_delta);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.lwip_udp_route_error_delta);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.lwip_ip_xmit_delta);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.lwip_ip_route_error_delta);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.lwip_ip_drop_delta);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.wifi_output_packets);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.wifi_output_bytes);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.wifi_output_ipv4_packets);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.wifi_output_udp_packets);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.wifi_output_arp_packets);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.wifi_linkoutput_calls);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.wifi_tx_queued);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.wifi_tx_confirmed);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.wifi_tx_acknowledged);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.wifi_tx_unacknowledged);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.last_udp_source_port);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.last_udp_destination_port);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.last_udp_ipv4_total_length);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.last_udp_length);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.last_udp_ipv4_checksum);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.last_udp_ipv4_checksum_residual);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.last_udp_checksum);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.last_udp_checksum_residual);
    (void)transport.write_cstr(",");
    write_u32(transport, diagnostics.last_udp_checksum_flags);
    (void)transport.write_cstr("\r\n");
}

} // namespace

UdpRuntimeDiagnostic::UdpRuntimeDiagnostic(
    UdpBackend backend,
    UdpRuntimeWaitHooks wait_hooks)
    : backend_(backend), wait_hooks_(wait_hooks)
{
}

bool UdpRuntimeDiagnostic::handles_command(const at::Command &command) const
{
    return is_name(command, kUdpRuntime);
}

at::CommandResult UdpRuntimeDiagnostic::handle_command(
    const at::Command &command,
    at::AtTransport &transport)
{
    unsigned long local_port = 0U;
    unsigned long remote_port = 0U;
    uint8_t remote_address[4] = {};
    UdpDiagnostics diagnostics = {};
    uint8_t source_address[4] = {};
    uint16_t source_port = 0U;
    size_t received = 0U;
    uint8_t attempts = 0U;
    UdpRuntimeStage stage = UdpRuntimeStage::Open;
    bool passed = false;
    bool trace_active = false;
    int handle = -1;

    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 3U ||
        backend_.open == nullptr ||
        backend_.close == nullptr ||
        backend_.send == nullptr ||
        backend_.receive == nullptr ||
        !parse_uint(command.arguments[0], 65535U, local_port) ||
        local_port == 0U ||
        !parse_uint(command.arguments[1], 65535U, remote_port) ||
        remote_port == 0U ||
        !parse_ipv4(command.arguments[2], remote_address)) {
        return at::CommandResult::Error;
    }

    if (!backend_.open(
            backend_.context,
            static_cast<uint16_t>(local_port),
            nullptr,
            &handle) ||
        handle < 0) {
        write_result(
            transport,
            false,
            stage,
            attempts,
            received,
            diagnostics,
            source_address,
            source_port);
        return at::CommandResult::Ok;
    }

    if (backend_.begin_runtime_trace != nullptr) {
        trace_active = backend_.begin_runtime_trace(backend_.context, handle);
    }

    stage = UdpRuntimeStage::Send;
    for (uint8_t attempt = 1U; attempt <= kSendAttempts && !passed; ++attempt) {
        size_t sent = 0U;
        attempts = attempt;
        if (!backend_.send(
                backend_.context,
                handle,
                remote_address,
                static_cast<uint16_t>(remote_port),
                kRequest,
                sizeof(kRequest) - 1U,
                &sent) ||
            sent != sizeof(kRequest) - 1U) {
            break;
        }

        stage = UdpRuntimeStage::ReceiveTimeout;
        const uint32_t started_at = now_ms();
        while (static_cast<uint32_t>(now_ms() - started_at) < kReceiveWindowMs) {
            uint8_t reply[sizeof(kExpectedReply)] = {};
            size_t reply_size = 0U;
            uint8_t reply_address[4] = {};
            uint16_t reply_port = 0U;
            if (!backend_.receive(
                    backend_.context,
                    handle,
                    reply,
                    sizeof(reply),
                    &reply_size,
                    reply_address,
                    &reply_port)) {
                stage = UdpRuntimeStage::Reply;
                break;
            }
            if (reply_size == 0U) {
                idle();
                continue;
            }

            received = reply_size;
            memcpy(source_address, reply_address, sizeof(source_address));
            source_port = reply_port;
            stage = UdpRuntimeStage::Reply;
            if (reply_size == sizeof(kExpectedReply) - 1U &&
                memcmp(reply, kExpectedReply, sizeof(kExpectedReply) - 1U) == 0 &&
                memcmp(reply_address, remote_address, sizeof(remote_address)) == 0 &&
                reply_port == static_cast<uint16_t>(remote_port)) {
                stage = UdpRuntimeStage::Passed;
                passed = true;
            }
            break;
        }
    }

    if (backend_.diagnostics != nullptr) {
        (void)backend_.diagnostics(backend_.context, handle, &diagnostics);
    }
    if (trace_active && backend_.end_runtime_trace != nullptr) {
        backend_.end_runtime_trace(backend_.context, handle);
    }
    (void)backend_.close(backend_.context, handle);
    write_result(
        transport,
        passed,
        stage,
        attempts,
        received,
        diagnostics,
        source_address,
        source_port);
    return at::CommandResult::Ok;
}

bool UdpRuntimeDiagnostic::parse_uint(
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

bool UdpRuntimeDiagnostic::parse_ipv4(const char *text, uint8_t address[4])
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

uint32_t UdpRuntimeDiagnostic::now_ms() const
{
    return wait_hooks_.now_ms == nullptr
        ? 0U
        : wait_hooks_.now_ms(wait_hooks_.context);
}

void UdpRuntimeDiagnostic::idle() const
{
    if (wait_hooks_.idle != nullptr) {
        wait_hooks_.idle(wait_hooks_.context);
    }
}

} // namespace bridge
