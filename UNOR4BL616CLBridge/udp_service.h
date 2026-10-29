#ifndef UNOR4_BL616CL_UDP_SERVICE_H
#define UNOR4_BL616CL_UDP_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "at_transport.h"
#include "network_scan_window.h"

namespace bridge {

constexpr size_t kUdpSlotCount = 4U;

struct UdpDiagnostics {
    uint32_t flags = 0U;
    int last_send_result = 0;
    int last_errno = 0;
    uint32_t send_heap_used_before = 0U;
    uint32_t send_heap_used_after = 0U;
    uint32_t send_heap_errors = 0U;
    uint32_t send_udp_xmit = 0U;
    uint32_t send_fragment_xmit = 0U;
    uint32_t send_fragment_errors = 0U;
    uint32_t fragment_confirmed = 0U;
    uint32_t fragment_acknowledged = 0U;
    uint32_t fragment_discarded = 0U;
    uint32_t fragment_content_active = 0U;
    uint32_t fragment_content_checked = 0U;
    uint32_t fragment_header_errors = 0U;
    uint32_t fragment_payload_errors = 0U;
    uint32_t fragment_content_mask = 0U;
    uint32_t fragment_identification = 0U;
    uint32_t fragment_udp_checksum_errors = 0U;
    uint32_t fragment_udp_checksum_residual = 0U;
    uint8_t sta_address[4] = {};
    uint8_t sta_gateway[4] = {};
    uint8_t default_address[4] = {};
    uint8_t local_address[4] = {};
    uint16_t local_port = 0U;
    uint32_t lwip_udp_xmit_delta = 0U;
    uint32_t lwip_udp_route_error_delta = 0U;
    uint32_t lwip_ip_xmit_delta = 0U;
    uint32_t lwip_ip_route_error_delta = 0U;
    uint32_t lwip_ip_drop_delta = 0U;
    uint32_t wifi_output_packets = 0U;
    uint32_t wifi_output_bytes = 0U;
    uint32_t wifi_output_ipv4_packets = 0U;
    uint32_t wifi_output_udp_packets = 0U;
    uint32_t wifi_output_arp_packets = 0U;
    uint32_t wifi_linkoutput_calls = 0U;
    uint32_t wifi_tx_queued = 0U;
    uint32_t wifi_tx_confirmed = 0U;
    uint32_t wifi_tx_acknowledged = 0U;
    uint32_t wifi_tx_unacknowledged = 0U;
    uint16_t last_udp_source_port = 0U;
    uint16_t last_udp_destination_port = 0U;
    uint16_t last_udp_ipv4_total_length = 0U;
    uint16_t last_udp_length = 0U;
    uint16_t last_udp_ipv4_checksum = 0U;
    uint16_t last_udp_ipv4_checksum_residual = 0U;
    uint16_t last_udp_checksum = 0U;
    uint16_t last_udp_checksum_residual = 0U;
    uint32_t last_udp_checksum_flags = 0U;
};

struct UdpBackend {
    void *context = nullptr;
    bool (*open)(
        void *context,
        uint16_t port,
        const uint8_t local_address[4],
        int *handle) = nullptr;
    bool (*join_multicast)(
        void *context,
        int handle,
        const uint8_t group_address[4]) = nullptr;
    bool (*close)(void *context, int handle) = nullptr;
    bool (*resolve_hostname)(
        void *context,
        const char *hostname,
        uint8_t address[4]) = nullptr;
    bool (*send)(
        void *context,
        int handle,
        const uint8_t destination[4],
        uint16_t port,
        const uint8_t *data,
        size_t size,
        size_t *sent) = nullptr;
    bool (*receive)(
        void *context,
        int handle,
        uint8_t *data,
        size_t capacity,
        size_t *received,
        uint8_t source_address[4],
        uint16_t *source_port) = nullptr;
    bool (*diagnostics)(
        void *context,
        int handle,
        UdpDiagnostics *diagnostics) = nullptr;
    bool (*begin_runtime_trace)(void *context, int handle) = nullptr;
    void (*end_runtime_trace)(void *context, int handle) = nullptr;
};

class UdpService {
public:
    explicit UdpService(UdpBackend backend = {});
    ~UdpService();
    void set_scan_control(NetworkScanWindow::Pause pause,
                          NetworkScanWindow::Resume resume, void *context);

    bool handles_command(const at::Command &command) const;
    at::CommandResult handle_command(
        const at::Command &command,
        at::AtTransport &transport);
    void close_all();

private:
    struct Slot {
        int handle = -1;
        uint8_t multicast_address[4] = {};
        uint16_t multicast_port = 0U;
        bool multicast = false;
        uint8_t remote_address[4] = {};
        uint16_t remote_port = 0U;
        bool remote_valid = false;
        uint8_t *received = nullptr;
        size_t received_size = 0U;
        size_t received_position = 0U;
    };

    at::CommandResult handle_begin(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_begin_multicast(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_begin_packet(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_begin_packet_multicast(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_begin_packet_ip(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_begin_packet_name(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_write(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_end_packet(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_parse(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_available(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_read(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_peek(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_flush(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_remote_ip(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_remote_port(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_debug(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_stop(
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
    static bool parse_ipv4(const char *text, uint8_t address[4]);
    bool slot_open(size_t slot) const;
    void close_slot(size_t slot);
    void discard_received(size_t slot);
    void release_packet();
    bool start_packet(
        size_t slot,
        const uint8_t destination[4],
        uint16_t port);
    static at::CommandResult append_payload(
        void *context,
        const uint8_t *data,
        size_t size);

    UdpBackend backend_;
    NetworkScanWindow::Pause scan_pause_ = nullptr;
    NetworkScanWindow::Resume scan_resume_ = nullptr;
    void *scan_control_context_ = nullptr;
    Slot slots_[kUdpSlotCount] = {};
    /* Two independent datagrams must remain available even when the heaps are
     * fragmented by concurrent TLS connections. */
    uint8_t received_storage_[at::kTransportRawPayloadCapacity] = {};
    size_t received_storage_slot_ = kUdpSlotCount;
    uint8_t packet_storage_[at::kTransportRawPayloadCapacity] = {};
    uint8_t *packet_buffer_ = nullptr;
    size_t packet_slot_ = kUdpSlotCount;
    size_t packet_size_ = 0U;
    uint8_t packet_destination_[4] = {};
    uint16_t packet_port_ = 0U;
};

const UdpBackend &bouffalo_udp_backend();

} // namespace bridge

#endif
