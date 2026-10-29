#include "udp_service.h"
#include "udp_socket_diagnostic.h"
#include "coex_diagnostic.h"
#include "dhcp_ack_packet.h"
#include "dhcp_tx_diagnostic.h"
#include "sta_tcp_diagnostic.h"
#include "udp_fragment_content_diagnostic.h"

#include <Arduino.h>
#include <FreeRTOS.h>
#include <string.h>

extern "C" {
#include "fhost.h"
#include "net_al_ext.h"
#include "wifi_mgmr_ext.h"
#include <lwip/errno.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/sockets.h>
#include <lwip/stats.h>
#include <lwip/tcpip.h>
#include <lwip/api.h>
#include <lwip/udp.h>

typedef void *(*bl_pkt_eth_output_hook_cb_t)(bool, void *, void *);
extern bl_pkt_eth_output_hook_cb_t bl_wifi_pkt_eth_output_hook;
extern void *bl_wifi_pkt_eth_output_hook_arg;
int fhost_tx_start(
    net_al_if_t net_if,
    net_al_tx_t net_buf,
    cb_fhost_tx cfm_cb,
    void *cfm_cb_arg);
}

namespace bridge {

namespace {

constexpr uint32_t kUdpDiagnosticSocketCreated = 1U << 0U;
constexpr uint32_t kUdpDiagnosticReuseAddress = 1U << 1U;
constexpr uint32_t kUdpDiagnosticStaName = 1U << 2U;
constexpr uint32_t kUdpDiagnosticStaBound = 1U << 3U;
constexpr uint32_t kUdpDiagnosticLocalBound = 1U << 4U;
constexpr uint32_t kUdpDiagnosticStaNetif = 1U << 5U;
constexpr uint32_t kUdpDiagnosticStaUp = 1U << 6U;
constexpr uint32_t kUdpDiagnosticStaLinkUp = 1U << 7U;
constexpr uint32_t kUdpDiagnosticDefaultIsSta = 1U << 8U;
constexpr uint32_t kUdpDiagnosticSocketLocal = 1U << 9U;
constexpr uint16_t kEthernetTypeArp = 0x0806U;
constexpr uint16_t kEthernetTypeIpv4 = 0x0800U;
constexpr size_t kEthernetHeaderBytes = 14U;
constexpr size_t kIpv4ProtocolOffset = kEthernetHeaderBytes + 9U;
constexpr uint8_t kIpv4ProtocolUdp = 17U;
constexpr size_t kIpv4MinimumHeaderBytes = 20U;
constexpr size_t kUdpHeaderBytes = 8U;
constexpr size_t kRuntimePacketDiagnosticBytes = 128U;
constexpr uint32_t kUdpChecksumObserved = 1U << 0U;
constexpr uint32_t kUdpIpv4ChecksumValid = 1U << 1U;
constexpr uint32_t kUdpChecksumPresent = 1U << 2U;
constexpr uint32_t kUdpChecksumValid = 1U << 3U;
constexpr size_t kUnfragmentedIpv4UdpPayloadBytes = 1500U - 20U - 8U;

// Passive counters for fragmented UDP during the most recent large send.
// No linkoutput replacement, payload retention, allocation or retries.
#if defined(BL616CL_UDP_FRAGMENT_CONTENT_DIAGNOSTIC) && BL616CL_UDP_FRAGMENT_CONTENT_DIAGNOSTIC
UdpFragmentContentDiagnostic fragment_content;
#endif
int fragment_observer_handle = -1;
uint32_t fragment_confirmed = 0U;
uint32_t fragment_acknowledged = 0U;
uint32_t fragment_discarded = 0U;

struct UdpTrace {
    uint32_t flags = 0U;
    int last_handle = -1;
    int last_send_result = 0;
    int last_errno = 0;
    uint32_t send_heap_used_before = 0U;
    uint32_t send_heap_used_after = 0U;
    uint32_t send_heap_errors = 0U;
    uint32_t send_udp_xmit = 0U;
    uint32_t send_fragment_xmit = 0U;
    uint32_t send_fragment_errors = 0U;
    struct {
        uint16_t calls = 0U;
        uint16_t would_block = 0U;
        uint16_t success = 0U;
        uint16_t failure = 0U;
        uint16_t restore_failure = 0U;
        uint16_t bytes = 0U;
        int16_t last_result = 0;
        int8_t last_errno = 0;
    } receive;
    struct {
        uint16_t attempts = 0U;
        uint8_t reject_mask = 0U;
        uint8_t mode = 0U;
        uint8_t argument_count = 0U;
        uint8_t slot = 0U;
        uint8_t slot_open = 0U;
        uint8_t backend_present = 0U;
    } parse;
    bool runtime_trace_active = false;
    struct netif *runtime_netif = nullptr;
    netif_linkoutput_fn original_linkoutput = nullptr;
    struct {
        uint16_t udp_xmit = 0U;
        uint16_t udp_recv = 0U;
        uint16_t udp_drop = 0U;
        uint16_t udp_checksum_error = 0U;
        uint16_t udp_length_error = 0U;
        uint16_t udp_protocol_error = 0U;
        uint16_t udp_route_error = 0U;
        uint16_t ip_xmit = 0U;
        uint16_t ip_route_error = 0U;
        uint16_t ip_drop = 0U;
    } lwip_start;
    struct {
        uint32_t packets = 0U;
        uint32_t bytes = 0U;
        uint32_t ipv4_packets = 0U;
        uint32_t udp_packets = 0U;
        uint32_t arp_packets = 0U;
    } wifi_output;
    struct {
        uint32_t linkoutput_calls = 0U;
        uint32_t queued = 0U;
        uint32_t confirmed = 0U;
        uint32_t acknowledged = 0U;
        uint32_t unacknowledged = 0U;
    } wifi_tx;
    struct {
        uint16_t source_port = 0U;
        uint16_t destination_port = 0U;
        uint16_t ipv4_total_length = 0U;
        uint16_t length = 0U;
        uint16_t ipv4_checksum = 0U;
        uint16_t ipv4_checksum_residual = 0U;
        uint16_t checksum = 0U;
        uint16_t checksum_residual = 0U;
        uint32_t checksum_flags = 0U;
    } last_udp;
};

UdpTrace udp_trace;

bool set_nonblocking(int handle, bool enabled)
{
    unsigned long nonblocking = enabled ? 1U : 0U;
    return ioctlsocket(handle, FIONBIO, &nonblocking) == 0;
}

bool active_ap_address(uint32_t *address)
{
    if (address == nullptr || !wifi_mgmr_ap_state_get()) {
        return false;
    }

    net_al_ext_ip_addr_cfg config = {};
    if (net_al_ext_get_vif_ip(MGMR_VIF_AP, &config) != 0 ||
        config.ipv4.addr == 0U) {
        return false;
    }

    *address = config.ipv4.addr;
    return true;
}

bool bind_socket_to_active_ap_interface(int handle)
{
    if (!wifi_mgmr_ap_state_get()) {
        return true;
    }

    struct ifreq request = {};
    const int name_length = fhost_vif_name(
        MGMR_VIF_AP,
        request.ifr_name,
        sizeof(request.ifr_name));
    if (name_length <= 0 ||
        name_length >= static_cast<int>(sizeof(request.ifr_name))) {
        errno = ENODEV;
        return false;
    }

    return setsockopt(
               handle,
               SOL_SOCKET,
               SO_BINDTODEVICE,
               &request,
               sizeof(request)) == 0;
}

void copy_ip_address(const ip4_addr_t *address, uint8_t output[4])
{
    if (address == nullptr || output == nullptr) {
        return;
    }
    const uint32_t value = ntohl(address->addr);
    output[0] = static_cast<uint8_t>(value >> 24U);
    output[1] = static_cast<uint8_t>(value >> 16U);
    output[2] = static_cast<uint8_t>(value >> 8U);
    output[3] = static_cast<uint8_t>(value);
}

uint16_t read_be16(const uint8_t data[2])
{
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(data[0]) << 8U) |
        static_cast<uint16_t>(data[1]));
}

void capture_lwip_start()
{
    udp_trace.lwip_start.udp_xmit = lwip_stats.udp.xmit;
    udp_trace.lwip_start.udp_recv = lwip_stats.udp.recv;
    udp_trace.lwip_start.udp_drop = lwip_stats.udp.drop;
    udp_trace.lwip_start.udp_checksum_error = lwip_stats.udp.chkerr;
    udp_trace.lwip_start.udp_length_error = lwip_stats.udp.lenerr;
    udp_trace.lwip_start.udp_protocol_error = lwip_stats.udp.proterr;
    udp_trace.lwip_start.udp_route_error = lwip_stats.udp.rterr;
    udp_trace.lwip_start.ip_xmit = lwip_stats.ip.xmit;
    udp_trace.lwip_start.ip_route_error = lwip_stats.ip.rterr;
    udp_trace.lwip_start.ip_drop = lwip_stats.ip.drop;
}

uint32_t counter_delta(uint16_t current, uint16_t start)
{
    return static_cast<uint16_t>(current - start);
}

uint16_t counter_delta_u16(uint16_t current, uint16_t start)
{
    return static_cast<uint16_t>(current - start);
}

void capture_runtime_deltas(UdpDiagnostics &diagnostics)
{
    if (!udp_trace.runtime_trace_active) {
        return;
    }
    diagnostics.lwip_udp_xmit_delta = counter_delta(
        lwip_stats.udp.xmit,
        udp_trace.lwip_start.udp_xmit);
    diagnostics.lwip_udp_route_error_delta = counter_delta(
        lwip_stats.udp.rterr,
        udp_trace.lwip_start.udp_route_error);
    diagnostics.lwip_ip_xmit_delta = counter_delta(
        lwip_stats.ip.xmit,
        udp_trace.lwip_start.ip_xmit);
    diagnostics.lwip_ip_route_error_delta = counter_delta(
        lwip_stats.ip.rterr,
        udp_trace.lwip_start.ip_route_error);
    diagnostics.lwip_ip_drop_delta = counter_delta(
        lwip_stats.ip.drop,
        udp_trace.lwip_start.ip_drop);
    diagnostics.wifi_output_packets = udp_trace.wifi_output.packets;
    diagnostics.wifi_output_bytes = udp_trace.wifi_output.bytes;
    diagnostics.wifi_output_ipv4_packets = udp_trace.wifi_output.ipv4_packets;
    diagnostics.wifi_output_udp_packets = udp_trace.wifi_output.udp_packets;
    diagnostics.wifi_output_arp_packets = udp_trace.wifi_output.arp_packets;
    taskENTER_CRITICAL();
    diagnostics.wifi_linkoutput_calls = udp_trace.wifi_tx.linkoutput_calls;
    diagnostics.wifi_tx_queued = udp_trace.wifi_tx.queued;
    diagnostics.wifi_tx_confirmed = udp_trace.wifi_tx.confirmed;
    diagnostics.wifi_tx_acknowledged = udp_trace.wifi_tx.acknowledged;
    diagnostics.wifi_tx_unacknowledged = udp_trace.wifi_tx.unacknowledged;
    diagnostics.last_udp_source_port = udp_trace.last_udp.source_port;
    diagnostics.last_udp_destination_port = udp_trace.last_udp.destination_port;
    diagnostics.last_udp_ipv4_total_length = udp_trace.last_udp.ipv4_total_length;
    diagnostics.last_udp_length = udp_trace.last_udp.length;
    diagnostics.last_udp_ipv4_checksum = udp_trace.last_udp.ipv4_checksum;
    diagnostics.last_udp_ipv4_checksum_residual =
        udp_trace.last_udp.ipv4_checksum_residual;
    diagnostics.last_udp_checksum = udp_trace.last_udp.checksum;
    diagnostics.last_udp_checksum_residual = udp_trace.last_udp.checksum_residual;
    diagnostics.last_udp_checksum_flags = udp_trace.last_udp.checksum_flags;
    taskEXIT_CRITICAL();
}

void capture_udp_socket_diagnostic(UdpSocketDiagnostic *diagnostic)
{
    if (diagnostic == nullptr) {
        return;
    }

    *diagnostic = {};
    if (udp_trace.last_handle < 0) {
        return;
    }

    struct netconn *connection = nullptr;
    socklen_t connection_length = sizeof(connection);
    if (getsockopt(
            udp_trace.last_handle,
            SOL_SOCKET,
            SO_CONNINFO,
            &connection,
            &connection_length) != 0) {
        diagnostic->socket_error = static_cast<int8_t>(errno);
    }

    LOCK_TCPIP_CORE();
    if (udp_trace.last_handle >= 0) {
        diagnostic->udp_recv_delta = counter_delta_u16(
            lwip_stats.udp.recv,
            udp_trace.lwip_start.udp_recv);
        diagnostic->udp_drop_delta = counter_delta_u16(
            lwip_stats.udp.drop,
            udp_trace.lwip_start.udp_drop);
        diagnostic->udp_checksum_error_delta = counter_delta_u16(
            lwip_stats.udp.chkerr,
            udp_trace.lwip_start.udp_checksum_error);
        diagnostic->udp_length_error_delta = counter_delta_u16(
            lwip_stats.udp.lenerr,
            udp_trace.lwip_start.udp_length_error);
        diagnostic->udp_protocol_error_delta = counter_delta_u16(
            lwip_stats.udp.proterr,
            udp_trace.lwip_start.udp_protocol_error);
        diagnostic->udp_route_error_delta = counter_delta_u16(
            lwip_stats.udp.rterr,
            udp_trace.lwip_start.udp_route_error);
        diagnostic->udp_xmit_delta = counter_delta_u16(
            lwip_stats.udp.xmit,
            udp_trace.lwip_start.udp_xmit);
    }

    for (struct udp_pcb *pcb = udp_get_pcbs();
         pcb != nullptr;
         pcb = pcb->next) {
        ++diagnostic->pcb_count;
        if (pcb->local_port == 4244U) {
            ++diagnostic->matching_port_pcb_count;
        }
        if (connection == nullptr || connection->pcb.udp != pcb) {
            continue;
        }
        diagnostic->socket_pcb_found = 1U;
        diagnostic->socket_pcb_netif_index = pcb->netif_idx;
        diagnostic->socket_pcb_flags = pcb->flags;
        diagnostic->socket_local_port = pcb->local_port;
        diagnostic->socket_remote_port = pcb->remote_port;
        diagnostic->socket_local_address = ip_2_ip4(&pcb->local_ip)->addr;
    }

    if (connection != nullptr) {
        diagnostic->socket_netconn_state =
            static_cast<uint8_t>(connection->state);
        diagnostic->socket_netconn_type =
            static_cast<uint8_t>(connection->type);
    }
    diagnostic->receive_call_count = udp_trace.receive.calls;
    diagnostic->receive_would_block_count = udp_trace.receive.would_block;
    diagnostic->receive_success_count = udp_trace.receive.success;
    diagnostic->receive_failure_count = udp_trace.receive.failure;
    diagnostic->receive_restore_failure_count =
        udp_trace.receive.restore_failure;
    diagnostic->receive_byte_count = udp_trace.receive.bytes;
    diagnostic->last_receive_result = udp_trace.receive.last_result;
    diagnostic->last_receive_errno = udp_trace.receive.last_errno;
    diagnostic->parse_attempt_count = udp_trace.parse.attempts;
    diagnostic->parse_reject_mask = udp_trace.parse.reject_mask;
    diagnostic->last_parse_mode = udp_trace.parse.mode;
    diagnostic->last_parse_argument_count = udp_trace.parse.argument_count;
    diagnostic->last_parse_slot = udp_trace.parse.slot;
    diagnostic->last_parse_slot_open = udp_trace.parse.slot_open;
    diagnostic->last_parse_backend_present = udp_trace.parse.backend_present;
    UNLOCK_TCPIP_CORE();
}

uint32_t internet_checksum_add(
    const uint8_t *data,
    size_t size,
    uint32_t sum = 0U)
{
    if (data == nullptr) {
        return sum;
    }
    while (size >= 2U) {
        sum += read_be16(data);
        data += 2U;
        size -= 2U;
    }
    if (size != 0U) {
        sum += static_cast<uint32_t>(data[0]) << 8U;
    }
    return sum;
}

uint16_t internet_checksum_residual(uint32_t sum)
{
    while ((sum >> 16U) != 0U) {
        sum = (sum & 0xFFFFU) + (sum >> 16U);
    }
    return static_cast<uint16_t>(sum);
}

void capture_udp_header(const uint8_t *packet, size_t copied)
{
    if (packet == nullptr ||
        copied < kEthernetHeaderBytes + kIpv4MinimumHeaderBytes ||
        read_be16(packet + 12U) != kEthernetTypeIpv4 ||
        packet[kIpv4ProtocolOffset] != kIpv4ProtocolUdp) {
        return;
    }

    const size_t ip_offset = kEthernetHeaderBytes;
    const size_t ip_header_bytes =
        static_cast<size_t>(packet[ip_offset] & 0x0FU) * 4U;
    if (ip_header_bytes < kIpv4MinimumHeaderBytes ||
        copied < ip_offset + ip_header_bytes + kUdpHeaderBytes) {
        return;
    }

    const size_t udp_offset = ip_offset + ip_header_bytes;
    const uint16_t udp_length = read_be16(packet + udp_offset + 4U);
    const bool complete_udp =
        udp_length >= kUdpHeaderBytes && copied >= udp_offset + udp_length;
    uint32_t flags = kUdpChecksumObserved;
    const uint16_t ipv4_residual = internet_checksum_residual(
        internet_checksum_add(packet + ip_offset, ip_header_bytes));
    if (ipv4_residual == 0xFFFFU) {
        flags |= kUdpIpv4ChecksumValid;
    }

    const uint16_t udp_checksum = read_be16(packet + udp_offset + 6U);
    uint16_t udp_residual = 0U;
    if (udp_checksum != 0U) {
        flags |= kUdpChecksumPresent;
        if (complete_udp) {
            uint32_t udp_sum = 0U;
            udp_sum = internet_checksum_add(packet + ip_offset + 12U, 8U, udp_sum);
            const uint8_t protocol[2] = { 0U, kIpv4ProtocolUdp };
            const uint8_t length[2] = {
                static_cast<uint8_t>(udp_length >> 8U),
                static_cast<uint8_t>(udp_length),
            };
            udp_sum = internet_checksum_add(protocol, sizeof(protocol), udp_sum);
            udp_sum = internet_checksum_add(length, sizeof(length), udp_sum);
            udp_sum = internet_checksum_add(packet + udp_offset, udp_length, udp_sum);
            udp_residual = internet_checksum_residual(udp_sum);
            if (udp_residual == 0xFFFFU) {
                flags |= kUdpChecksumValid;
            }
        }
    } else {
        flags |= kUdpChecksumValid;
    }

    taskENTER_CRITICAL();
    udp_trace.last_udp.source_port = read_be16(packet + udp_offset);
    udp_trace.last_udp.destination_port = read_be16(packet + udp_offset + 2U);
    udp_trace.last_udp.ipv4_total_length = read_be16(packet + ip_offset + 2U);
    udp_trace.last_udp.length = udp_length;
    udp_trace.last_udp.ipv4_checksum = read_be16(packet + ip_offset + 10U);
    udp_trace.last_udp.ipv4_checksum_residual = ipv4_residual;
    udp_trace.last_udp.checksum = udp_checksum;
    udp_trace.last_udp.checksum_residual = udp_residual;
    udp_trace.last_udp.checksum_flags = flags;
    taskEXIT_CRITICAL();
}

void capture_wifi_output_packet(struct pbuf *buffer)
{
    if (buffer == nullptr) {
        return;
    }

    ++udp_trace.wifi_output.packets;
    udp_trace.wifi_output.bytes += buffer->tot_len;

    uint8_t header[kRuntimePacketDiagnosticBytes] = {};
    const u16_t copied = pbuf_copy_partial(
        buffer,
        header,
        sizeof(header),
        0U);
    if (copied < kEthernetHeaderBytes) {
        return;
    }

    const uint16_t type = read_be16(header + 12U);
    if (type == kEthernetTypeArp) {
        ++udp_trace.wifi_output.arp_packets;
    } else if (type == kEthernetTypeIpv4) {
        ++udp_trace.wifi_output.ipv4_packets;
        if (copied > kIpv4ProtocolOffset &&
            header[kIpv4ProtocolOffset] == kIpv4ProtocolUdp) {
            ++udp_trace.wifi_output.udp_packets;
            capture_udp_header(header, copied);
        }
    }
}

void runtime_tx_confirmation(uint32_t, bool acknowledged, void *)
{
    taskENTER_CRITICAL();
    if (udp_trace.runtime_trace_active) {
        ++udp_trace.wifi_tx.confirmed;
        if (acknowledged) {
            ++udp_trace.wifi_tx.acknowledged;
        } else {
            ++udp_trace.wifi_tx.unacknowledged;
        }
    }
    taskEXIT_CRITICAL();
}

err_t runtime_linkoutput(struct netif *netif, struct pbuf *buffer)
{
    if (!udp_trace.runtime_trace_active || netif != udp_trace.runtime_netif ||
        buffer == nullptr) {
        return ERR_IF;
    }

    taskENTER_CRITICAL();
    ++udp_trace.wifi_tx.linkoutput_calls;
    taskEXIT_CRITICAL();

    /*
     * This mirrors the bridge runtime's current SDK net_if_output() path.
     * Do not replace the SDK's optional global output hook: invoke it here
     * exactly as net_if_output() would, then queue the resulting pbuf with a
     * TX confirmation callback.
     */
    if (bl_wifi_pkt_eth_output_hook != nullptr) {
        buffer = static_cast<struct pbuf *>(bl_wifi_pkt_eth_output_hook(
            true,
            buffer,
            bl_wifi_pkt_eth_output_hook_arg));
        if (buffer == nullptr) {
            return ERR_BUF;
        }
    }

    capture_wifi_output_packet(buffer);
    pbuf_ref(buffer);
    if (netif_is_up(netif) &&
        fhost_tx_start(netif, buffer, runtime_tx_confirmation, nullptr) == 0) {
        taskENTER_CRITICAL();
        ++udp_trace.wifi_tx.queued;
        taskEXIT_CRITICAL();
        return ERR_OK;
    }

    pbuf_free(buffer);
    return ERR_BUF;
}

bool open_udp_socket(
    void *,
    uint16_t port,
    const uint8_t local_address[4],
    int *handle)
{
    if (handle == nullptr || port == 0U) {
        return false;
    }
    udp_trace = {};
    /*
     * Match the Bouffalo SDK UDP examples exactly. lwIP selects UDP for a
     * SOCK_DGRAM socket when protocol is zero.
     */
    const int socket_handle = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_handle < 0) {
        udp_trace.last_errno = errno;
        return false;
    }
    udp_trace.flags |= kUdpDiagnosticSocketCreated;
    udp_trace.last_handle = socket_handle;

    int reuse_address = 1;
    if (setsockopt(
            socket_handle,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse_address,
            sizeof(reuse_address)) != 0) {
        udp_trace.last_errno = errno;
        (void)closesocket(socket_handle);
        return false;
    }
    udp_trace.flags |= kUdpDiagnosticReuseAddress;
    struct sockaddr_in local = {};
    local.sin_len = sizeof(local);
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    if (local_address != nullptr) {
        local.sin_addr.s_addr = htonl(
            (static_cast<uint32_t>(local_address[0]) << 24U) |
            (static_cast<uint32_t>(local_address[1]) << 16U) |
            (static_cast<uint32_t>(local_address[2]) << 8U) |
            static_cast<uint32_t>(local_address[3]));
    } else if (!active_ap_address(&local.sin_addr.s_addr)) {
        local.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    if (bind(
            socket_handle,
            reinterpret_cast<struct sockaddr *>(&local),
            sizeof(local)) != 0) {
        udp_trace.last_errno = errno;
        (void)closesocket(socket_handle);
        return false;
    }
    udp_trace.flags |= kUdpDiagnosticLocalBound;
    /*
     * TCP servers already select the AP address and VIF in AP+STA mode.
     * A WiFiUDP::begin(port) socket has the same role for an AP client, but
     * a wildcard UDP PCB otherwise follows the STA/default route. Bind it
     * after bind() so inbound and reply datagrams stay on the AP VIF.
     */
    uint32_t ap_address = 0U;
    const bool use_ap_interface = local_address == nullptr ||
        (active_ap_address(&ap_address) && local.sin_addr.s_addr == ap_address);
    // An explicit STA bind must keep its chosen source/interface in AP+STA.
    if (use_ap_interface && !bind_socket_to_active_ap_interface(socket_handle)) {
        udp_trace.last_errno = errno;
        (void)closesocket(socket_handle);
        return false;
    }
    LOCK_TCPIP_CORE();
    capture_lwip_start();
    UNLOCK_TCPIP_CORE();
    *handle = socket_handle;
    return true;
}

bool join_multicast(
    void *,
    int handle,
    const uint8_t group_address[4])
{
    if (group_address == nullptr) {
        return false;
    }
    struct ip_mreq membership = {};
    membership.imr_multiaddr.s_addr = htonl(
        (static_cast<uint32_t>(group_address[0]) << 24U) |
        (static_cast<uint32_t>(group_address[1]) << 16U) |
        (static_cast<uint32_t>(group_address[2]) << 8U) |
        static_cast<uint32_t>(group_address[3]));
    membership.imr_interface.s_addr = htonl(INADDR_ANY);
    return setsockopt(
        handle,
        IPPROTO_IP,
        IP_ADD_MEMBERSHIP,
        &membership,
        sizeof(membership)) == 0;
}

bool close_udp_socket(void *, int handle)
{
    taskENTER_CRITICAL();
    if (fragment_observer_handle == handle) fragment_observer_handle = -1;
    taskEXIT_CRITICAL();
    return closesocket(handle) == 0;
}

bool resolve_hostname(void *, const char *hostname, uint8_t address[4])
{
    if (hostname == nullptr || address == nullptr) {
        return false;
    }
    struct hostent *host = gethostbyname(hostname);
    if (host == nullptr || host->h_addr_list == nullptr ||
        host->h_addr_list[0] == nullptr || host->h_length < 4) {
        return false;
    }
    memcpy(address, host->h_addr_list[0], 4U);
    return true;
}

bool send_datagram(
    void *,
    int handle,
    const uint8_t destination[4],
    uint16_t port,
    const uint8_t *data,
    size_t size,
    size_t *sent)
{
    if (destination == nullptr || sent == nullptr || port == 0U ||
        (data == nullptr && size != 0U)) {
        return false;
    }
    struct sockaddr_in remote = {};
    remote.sin_len = sizeof(remote);
    remote.sin_family = AF_INET;
    remote.sin_port = htons(port);
    remote.sin_addr.s_addr = htonl(
        (static_cast<uint32_t>(destination[0]) << 24U) |
        (static_cast<uint32_t>(destination[1]) << 16U) |
        (static_cast<uint32_t>(destination[2]) << 8U) |
        static_cast<uint32_t>(destination[3]));
    /*
     * Keep datagram sockets blocking by default. The SDK's UDP examples use
     * this model, and a UDP send is atomic: a short successful send is not
     * retried because that could duplicate a packet.
     */
    const uint32_t heap_used_before = lwip_stats.mem.used;
    const uint16_t heap_errors_before = lwip_stats.mem.err;
    const uint16_t udp_xmit_before = lwip_stats.udp.xmit;
    const uint16_t fragment_xmit_before = lwip_stats.ip_frag.xmit;
    const uint16_t fragment_errors_before = lwip_stats.ip_frag.memerr;
    const int flags = size > kUnfragmentedIpv4UdpPayloadBytes
        ? MSG_BL616CL_UDP_REF
        : 0;
    if (size > kUnfragmentedIpv4UdpPayloadBytes) {
        taskENTER_CRITICAL();
        fragment_observer_handle = handle;
        fragment_confirmed = fragment_acknowledged = fragment_discarded = 0U;
        taskEXIT_CRITICAL();
    }
#if defined(BL616CL_UDP_FRAGMENT_CONTENT_DIAGNOSTIC) && BL616CL_UDP_FRAGMENT_CONTENT_DIAGNOSTIC
    if (size > kUnfragmentedIpv4UdpPayloadBytes) {
        taskENTER_CRITICAL();
        fragment_content.prepare(data, size, destination, port);
        taskEXIT_CRITICAL();
    }
#endif
    const int result = sendto(
        handle,
        data,
        size,
        flags,
        reinterpret_cast<struct sockaddr *>(&remote),
        sizeof(remote));
    udp_trace.last_handle = handle;
    udp_trace.last_send_result = result;
    udp_trace.last_errno = result < 0 ? errno : 0;
    udp_trace.send_heap_used_before = heap_used_before;
    udp_trace.send_heap_used_after = lwip_stats.mem.used;
    udp_trace.send_heap_errors = static_cast<uint16_t>(
        lwip_stats.mem.err - heap_errors_before);
    udp_trace.send_udp_xmit = static_cast<uint16_t>(
        lwip_stats.udp.xmit - udp_xmit_before);
    udp_trace.send_fragment_xmit = static_cast<uint16_t>(
        lwip_stats.ip_frag.xmit - fragment_xmit_before);
    udp_trace.send_fragment_errors = static_cast<uint16_t>(
        lwip_stats.ip_frag.memerr - fragment_errors_before);
    if (result < 0) {
        *sent = 0U;
        return false;
    }
    *sent = static_cast<size_t>(result);
    return *sent == size;
}

bool receive_datagram(
    void *,
    int handle,
    uint8_t *data,
    size_t capacity,
    size_t *received,
    uint8_t source_address[4],
    uint16_t *source_port)
{
    if (data == nullptr || received == nullptr || source_address == nullptr ||
        source_port == nullptr || capacity == 0U) {
        return false;
    }
    ++udp_trace.receive.calls;
    /*
     * UDPPARSE is a readiness poll from WiFiS3, so it must not block the AT
     * task when no datagram is queued. Restore the socket's normal blocking
     * mode immediately afterwards so sendto follows the SDK's UDP path.
     */
    if (!set_nonblocking(handle, true)) {
        ++udp_trace.receive.failure;
        udp_trace.receive.last_errno = static_cast<int8_t>(errno);
        return false;
    }
    struct sockaddr_in source = {};
    socklen_t source_length = sizeof(source);
    const int result = recvfrom(
        handle,
        data,
        capacity,
        0,
        reinterpret_cast<struct sockaddr *>(&source),
        &source_length);
    const int socket_errno = errno;
    if (!set_nonblocking(handle, false)) {
        ++udp_trace.receive.restore_failure;
        udp_trace.receive.last_result = static_cast<int16_t>(result);
        udp_trace.receive.last_errno = static_cast<int8_t>(errno);
        return false;
    }
    udp_trace.receive.last_result = static_cast<int16_t>(result);
    udp_trace.receive.last_errno = static_cast<int8_t>(
        result < 0 ? socket_errno : 0);
    if (result < 0) {
        if (socket_errno == EAGAIN || socket_errno == EWOULDBLOCK) {
            ++udp_trace.receive.would_block;
            *received = 0U;
            return true;
        }
        ++udp_trace.receive.failure;
        return false;
    }
    const uint32_t address = ntohl(source.sin_addr.s_addr);
    source_address[0] = static_cast<uint8_t>(address >> 24U);
    source_address[1] = static_cast<uint8_t>(address >> 16U);
    source_address[2] = static_cast<uint8_t>(address >> 8U);
    source_address[3] = static_cast<uint8_t>(address);
    *source_port = ntohs(source.sin_port);
    *received = static_cast<size_t>(result);
    ++udp_trace.receive.success;
    udp_trace.receive.bytes = static_cast<uint16_t>(
        udp_trace.receive.bytes + static_cast<uint16_t>(result));
    return true;
}

bool get_udp_diagnostics(void *, int handle, UdpDiagnostics *diagnostics)
{
    if (diagnostics == nullptr) {
        return false;
    }
    *diagnostics = {};
    diagnostics->flags = udp_trace.flags;
    if (udp_trace.last_handle == handle) {
        diagnostics->last_send_result = udp_trace.last_send_result;
        diagnostics->last_errno = udp_trace.last_errno;
        diagnostics->send_heap_used_before = udp_trace.send_heap_used_before;
        diagnostics->send_heap_used_after = udp_trace.send_heap_used_after;
        diagnostics->send_heap_errors = udp_trace.send_heap_errors;
        diagnostics->send_udp_xmit = udp_trace.send_udp_xmit;
        diagnostics->send_fragment_xmit = udp_trace.send_fragment_xmit;
        diagnostics->send_fragment_errors = udp_trace.send_fragment_errors;
        taskENTER_CRITICAL();
        if (fragment_observer_handle == handle) {
            diagnostics->fragment_confirmed = fragment_confirmed;
            diagnostics->fragment_acknowledged = fragment_acknowledged;
            diagnostics->fragment_discarded = fragment_discarded;
#if defined(BL616CL_UDP_FRAGMENT_CONTENT_DIAGNOSTIC) && BL616CL_UDP_FRAGMENT_CONTENT_DIAGNOSTIC
            diagnostics->fragment_content_active = fragment_content.active;
            diagnostics->fragment_content_checked = fragment_content.checked;
            diagnostics->fragment_header_errors = fragment_content.header_errors;
            diagnostics->fragment_payload_errors = fragment_content.payload_errors;
            diagnostics->fragment_content_mask = fragment_content.mask;
            diagnostics->fragment_identification = fragment_content.identification;
            diagnostics->fragment_udp_checksum_errors = fragment_content.udp_checksum_errors;
            diagnostics->fragment_udp_checksum_residual = fragment_content.udp_checksum_residual;
#endif
        }
        taskEXIT_CRITICAL();
    }
    LOCK_TCPIP_CORE();
    capture_runtime_deltas(*diagnostics);
    UNLOCK_TCPIP_CORE();

    struct netif *sta = static_cast<struct netif *>(fhost_to_net_if(MGMR_VIF_STA));
    if (sta != nullptr) {
        diagnostics->flags |= kUdpDiagnosticStaNetif;
        copy_ip_address(netif_ip4_addr(sta), diagnostics->sta_address);
        copy_ip_address(netif_ip4_gw(sta), diagnostics->sta_gateway);
        if (netif_is_up(sta)) {
            diagnostics->flags |= kUdpDiagnosticStaUp;
        }
        if (netif_is_link_up(sta)) {
            diagnostics->flags |= kUdpDiagnosticStaLinkUp;
        }
        if (netif_default == sta) {
            diagnostics->flags |= kUdpDiagnosticDefaultIsSta;
        }
    }
    if (netif_default != nullptr) {
        copy_ip_address(netif_ip4_addr(netif_default), diagnostics->default_address);
    }

    struct sockaddr_in local = {};
    socklen_t local_length = sizeof(local);
    if (getsockname(
            handle,
            reinterpret_cast<struct sockaddr *>(&local),
            &local_length) != 0) {
        diagnostics->last_errno = errno;
        return false;
    }
    diagnostics->flags |= kUdpDiagnosticSocketLocal;
    const uint32_t address = ntohl(local.sin_addr.s_addr);
    diagnostics->local_address[0] = static_cast<uint8_t>(address >> 24U);
    diagnostics->local_address[1] = static_cast<uint8_t>(address >> 16U);
    diagnostics->local_address[2] = static_cast<uint8_t>(address >> 8U);
    diagnostics->local_address[3] = static_cast<uint8_t>(address);
    diagnostics->local_port = ntohs(local.sin_port);
    return true;
}

bool begin_runtime_trace(void *, int handle)
{
    if (udp_trace.last_handle != handle || udp_trace.runtime_trace_active) {
        return false;
    }
    LOCK_TCPIP_CORE();
    struct netif *sta = static_cast<struct netif *>(fhost_to_net_if(MGMR_VIF_STA));
    if (sta == nullptr || sta->linkoutput == nullptr ||
        sta->linkoutput == runtime_linkoutput) {
        UNLOCK_TCPIP_CORE();
        return false;
    }
    capture_lwip_start();
    udp_trace.wifi_output = {};
    udp_trace.wifi_tx = {};
    udp_trace.last_udp = {};
    udp_trace.runtime_netif = sta;
    udp_trace.original_linkoutput = sta->linkoutput;
    sta->linkoutput = runtime_linkoutput;
    udp_trace.runtime_trace_active = true;
    UNLOCK_TCPIP_CORE();
    return true;
}

void end_runtime_trace(void *, int handle)
{
    if (udp_trace.last_handle != handle || !udp_trace.runtime_trace_active) {
        return;
    }
    LOCK_TCPIP_CORE();
    if (udp_trace.runtime_netif != nullptr &&
        udp_trace.runtime_netif->linkoutput == runtime_linkoutput) {
        udp_trace.runtime_netif->linkoutput = udp_trace.original_linkoutput;
    }
    udp_trace.original_linkoutput = nullptr;
    udp_trace.runtime_netif = nullptr;
    udp_trace.runtime_trace_active = false;
    UNLOCK_TCPIP_CORE();
}

} // namespace

static bool bridge_wifi_buffer_is_tcp_syn(uintptr_t buffer, uint8_t *destination = nullptr,
                                        uint8_t *source = nullptr)
{
    if (buffer != 0U) {
        uint8_t header[64] = {};
        const uint16_t offsets[] = {0U, PBUF_LINK_ENCAPSULATION_HLEN};
        for (const uint16_t offset : offsets) {
            const size_t copied = pbuf_copy_partial(
                reinterpret_cast<struct pbuf *>(buffer), header, sizeof(header), offset);
            const size_t ip_header = (header[14U] & 0x0FU) * 4U;
            const size_t tcp_flags = 14U + ip_header + 13U;
            if (copied > tcp_flags && ip_header >= 20U &&
                read_be16(header + 12U) == kEthernetTypeIpv4 &&
                (header[14U] >> 4U) == 4U && header[kIpv4ProtocolOffset] == 6U &&
                (read_be16(header + 20U) & 0x3FFFU) == 0U &&
                (header[tcp_flags] & 0x12U) == 0x02U) {
                if (destination != nullptr) memcpy(destination, header, 6U);
                if (source != nullptr) memcpy(source, header + 6U, 6U);
                return true;
            }
        }
    }
    return false;
}

extern "C" void bridge_wifi_tx_release_observer(
    uintptr_t buffer, uint32_t acknowledged, uint32_t discarded)
{
    taskENTER_CRITICAL();
#if BL616CL_STA_TCP_DIAGNOSTIC
    sta_tcp_tx_release(reinterpret_cast<void *>(buffer), acknowledged, discarded);
#endif
#if BL616CL_DHCP_TX_DIAGNOSTIC
    if (buffer != 0U) {
        const uint16_t offsets[] = {0U, PBUF_LINK_ENCAPSULATION_HLEN};
        for (const uint16_t offset : offsets) {
            uint8_t header[14U + 60U + 8U] = {};
            const size_t size = pbuf_copy_partial(
                reinterpret_cast<struct pbuf *>(buffer), header, sizeof(header), offset);
            if (size < 42U || header[12] != 8U || header[13] != 0U ||
                (header[14] >> 4U) != 4U || header[23] != 17U) continue;
            const size_t udp = 14U + (header[14] & 15U)*4U;
            if (udp < 34U || size < udp + 8U ||
                header[udp] != 0U || header[udp+1U] != 67U ||
                header[udp+2U] != 0U || header[udp+3U] != 68U) continue;
            uint8_t prefix[14U + 60U + 8U + 240U + 64U] = {};
            const size_t copied = pbuf_copy_partial(
                reinterpret_cast<struct pbuf *>(buffer), prefix, sizeof(prefix), offset);
            const uint8_t type = dhcp_server_message_type(prefix, copied);
            if (type != 0U) {
                dhcp_tx_diagnostic_note(type, acknowledged, discarded);
                break;
            }
        }
    }
#endif
    if (ap_join_scan_counters().active.load() != 0U && discarded == 0U && buffer != 0U) {
        uint8_t prefix[14U + 60U + 8U + 240U + 64U] = {};
        const uint16_t offsets[] = {0U, PBUF_LINK_ENCAPSULATION_HLEN};
        for (const uint16_t offset : offsets) {
            const size_t copied = pbuf_copy_partial(
                reinterpret_cast<struct pbuf *>(buffer), prefix, sizeof(prefix), offset);
            if (is_dhcp_ack_packet(prefix, copied)) {
                // Broadcast completion does not prove client receipt. The
                // host gate independently requires DHCP and peer traffic.
                ++ap_join_scan_counters().dhcp_ack_transmitted;
                break;
            }
            if (acknowledged != 0U && is_ap_client_ip_reply(
                    prefix, copied, ap_join_scan_counters().ap_ipv4.load())) {
                // A non-DHCP IP reply from the AP was delivered to a client's
                // MAC. This follows client IP traffic, unlike a DHCP broadcast.
                ++ap_join_scan_counters().client_ip_reply_acknowledged;
                break;
            }
        }
    }
    // Reuse the existing passive pre-release hook for outbound TCP SYNs.
    // MAC acknowledgement is link delivery, not a completed TCP handshake.
    if (bridge_wifi_buffer_is_tcp_syn(buffer)) {
        ++wifi_syn_counters().confirmed;
        if (acknowledged != 0U) ++wifi_syn_counters().acknowledged;
        if (discarded != 0U) ++wifi_syn_counters().discarded;
    }
    if (fragment_observer_handle >= 0 && buffer != 0U) {
        uint8_t header[kEthernetHeaderBytes + kIpv4MinimumHeaderBytes] = {};
        // net_buf_tx_info() reveals the reserved FHOST headroom and leaves
        // payload there until release. Early rejects can precede that step.
        const uint16_t offsets[] = {0U, PBUF_LINK_ENCAPSULATION_HLEN};
        for (const uint16_t offset : offsets) {
            const size_t copied = pbuf_copy_partial(
                reinterpret_cast<struct pbuf *>(buffer), header, sizeof(header), offset);
            if (copied == sizeof(header) && read_be16(header + 12U) == kEthernetTypeIpv4 &&
                (header[14U] >> 4U) == 4U && header[kIpv4ProtocolOffset] == kIpv4ProtocolUdp &&
                (read_be16(header + 20U) & 0x3FFFU) != 0U) {
#if defined(BL616CL_UDP_FRAGMENT_CONTENT_DIAGNOSTIC) && BL616CL_UDP_FRAGMENT_CONTENT_DIAGNOSTIC
                fragment_content.observe([buffer](uint8_t *out, size_t length, size_t position) {
                    return static_cast<size_t>(pbuf_copy_partial(
                        reinterpret_cast<struct pbuf *>(buffer), out, length, position));
                }, offset);
#endif
                ++fragment_confirmed;
                if (acknowledged != 0U) ++fragment_acknowledged;
                if (discarded != 0U) ++fragment_discarded;
                break;
            }
        }
    }
    taskEXIT_CRITICAL();
}

const UdpBackend &bouffalo_udp_backend()
{
    static const UdpBackend backend = {
        nullptr,
        open_udp_socket,
        join_multicast,
        close_udp_socket,
        resolve_hostname,
        send_datagram,
        receive_datagram,
        get_udp_diagnostics,
        begin_runtime_trace,
        end_runtime_trace,
    };
    return backend;
}

UdpSocketDiagnostic udp_socket_diagnostic_snapshot()
{
    UdpSocketDiagnostic diagnostic = {};
    capture_udp_socket_diagnostic(&diagnostic);
    return diagnostic;
}

void udp_socket_diagnostic_note_parse(
    uint8_t mode,
    uint8_t argument_count,
    uint8_t slot,
    bool slot_valid,
    bool slot_open,
    bool backend_present)
{
    ++udp_trace.parse.attempts;
    udp_trace.parse.mode = mode;
    udp_trace.parse.argument_count = argument_count;
    udp_trace.parse.slot = slot;
    udp_trace.parse.slot_open = slot_open ? 1U : 0U;
    udp_trace.parse.backend_present = backend_present ? 1U : 0U;
    udp_trace.parse.reject_mask =
        (mode == static_cast<uint8_t>(at::CommandMode::Write) ? 0U : 0x01U) |
        (argument_count == 1U ? 0U : 0x02U) |
        (slot_valid ? 0U : 0x04U) |
        (slot_open ? 0U : 0x08U) |
        (backend_present ? 0U : 0x10U);
}

// Optional newer runtime hook. Older bundles simply leave the site at zero.
extern "C" void bridge_wifi_tx_discard_site_observer(uintptr_t buffer, uintptr_t site)
{
    taskENTER_CRITICAL();
    if (bridge_wifi_buffer_is_tcp_syn(buffer, wifi_syn_counters().last_discard_destination,
                                    wifi_syn_counters().last_discard_source)) {
        wifi_syn_counters().last_discard_site.store(static_cast<uint32_t>(site));
    }
    taskEXIT_CRITICAL();
}

} // namespace bridge
