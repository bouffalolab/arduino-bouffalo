#include "tcp_client_service.h"
#include "tcp_runtime_diagnostic.h"
#include "tcp_rx_native_policy.h"

#include <Arduino.h>
#include <string.h>

extern "C" {
#if BL616CL_TCP_RX_BUDGET_DIAGNOSTIC
#include <lwip/priv/tcp_priv.h>
#include <lwip/stats.h>
#include <lwip/tcpip.h>
#endif
#include <lwip/dns.h>
#include <lwip/errno.h>
#include <lwip/inet.h>
#include <lwip/api.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <net_al.h>
#include <net_al_ext.h>
#include <fhost.h>
#include <wifi_mgmr_ext.h>
}

namespace bridge {

namespace {

constexpr uint32_t kSocketIoTimeoutMs = 5000U;
TcpHostnameDiagnostic hostname_diagnostic;

#ifndef BL616CL_APSTA_LISTENER_BIND_DEVICE
#define BL616CL_APSTA_LISTENER_BIND_DEVICE 1
#endif

bool wait_writable(int handle, uint32_t timeout_ms)
{
    fd_set writable = {};
    FD_SET(handle, &writable);
    struct timeval timeout = {
        static_cast<long>(timeout_ms / 1000U),
        static_cast<long>((timeout_ms % 1000U) * 1000U),
    };
    return select(handle + 1, nullptr, &writable, nullptr, &timeout) > 0 &&
        FD_ISSET(handle, &writable);
}

bool set_socket_timeouts(int handle, uint32_t timeout_ms)
{
    struct timeval timeout = {
        static_cast<long>(timeout_ms / 1000U),
        static_cast<long>((timeout_ms % 1000U) * 1000U),
    };
    return setsockopt(
               handle,
               SOL_SOCKET,
               SO_RCVTIMEO,
               &timeout,
               sizeof(timeout)) == 0 &&
        setsockopt(
               handle,
               SOL_SOCKET,
               SO_SNDTIMEO,
               &timeout,
               sizeof(timeout)) == 0;
}

bool set_nonblocking(int handle, bool enabled)
{
    unsigned long nonblocking = enabled ? 1U : 0U;
    return ioctlsocket(handle, FIONBIO, &nonblocking) == 0;
}

enum class SocketPeerState {
    Alive,
    Closed,
    ProbeFailure,
};

constexpr size_t kRememberedClosedSocketCount = 32U;
int remembered_closed_handles[kRememberedClosedSocketCount] = {};
size_t remembered_closed_handle_count = 0U;

bool remembered_closed_handle(int handle)
{
    for (size_t index = 0U; index < remembered_closed_handle_count; ++index) {
        if (remembered_closed_handles[index] == handle) {
            return true;
        }
    }
    return false;
}

void remember_closed_handle(int handle)
{
    if (handle < 0 || remembered_closed_handle(handle) ||
        remembered_closed_handle_count >= kRememberedClosedSocketCount) {
        return;
    }
    remembered_closed_handles[remembered_closed_handle_count++] = handle;
}

void forget_closed_handle(int handle)
{
    for (size_t index = 0U; index < remembered_closed_handle_count; ++index) {
        if (remembered_closed_handles[index] != handle) {
            continue;
        }
        remembered_closed_handles[index] =
            remembered_closed_handles[remembered_closed_handle_count - 1U];
        --remembered_closed_handle_count;
        return;
    }
}

SocketPeerState probe_socket_peer(int handle)
{
    if (remembered_closed_handle(handle)) {
        return SocketPeerState::Closed;
    }
    if (!set_nonblocking(handle, true)) {
        return SocketPeerState::ProbeFailure;
    }

    int socket_error = 0;
    socklen_t socket_error_length = sizeof(socket_error);
    if (getsockopt(
            handle,
            SOL_SOCKET,
            SO_ERROR,
            &socket_error,
            &socket_error_length) != 0) {
        return SocketPeerState::ProbeFailure;
    }
    if (socket_error != 0) {
        remember_closed_handle(handle);
        return SocketPeerState::Closed;
    }

    uint8_t byte = 0U;
    const int result = recv(handle, &byte, 1U, MSG_PEEK);
    if (result > 0) {
        return SocketPeerState::Alive;
    }
    if (result == 0) {
        remember_closed_handle(handle);
        return SocketPeerState::Closed;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return SocketPeerState::Alive;
    }
    remember_closed_handle(handle);
    return SocketPeerState::Closed;
}

bool create_socket(void *, int *handle)
{
    if (handle == nullptr) {
        return false;
    }
    const int socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle < 0) {
        return false;
    }
    *handle = socket_handle;
    forget_closed_handle(socket_handle);
    return true;
}

bool close_socket(void *, int handle)
{
    const int result = closesocket(handle);
    forget_closed_handle(handle);
    tcp_runtime_diagnostic_note_close_result(
        handle,
        result,
        result == 0 ? 0 : errno);
    return result == 0;
}

void close_listener_setup_failure(int listener)
{
    tcp_runtime_diagnostic_note_close_request(
        listener,
        TcpCloseOrigin::ListenerSetupFailure);
    (void)close_socket(nullptr, listener);
}

uint32_t listener_bind_address()
{
    /*
     * In AP+STA mode the SDK's own multi-VIF iperf service binds servers to
     * the selected VIF address. A wildcard BSD socket can be matched against
     * the STA/default netif before AP input reaches it, which leaves an AP
     * client seeing ConnectionRefused even though listen() succeeded.
     *
     * Preserve INADDR_ANY for STA-only servers. Once the AP VIF is active,
     * use its configured IPv4 address so lwIP has an unambiguous local PCB
     * match for packets arriving from an AP station.
     */
    if (!wifi_mgmr_ap_state_get()) {
        return htonl(INADDR_ANY);
    }

    net_al_ext_ip_addr_cfg ap_config = {};
    if (net_al_ext_get_vif_ip(MGMR_VIF_AP, &ap_config) != 0 ||
        ap_config.ipv4.addr == 0U) {
        return htonl(INADDR_ANY);
    }
    return ap_config.ipv4.addr;
}

int bind_listener_to_active_ap_interface(int listener)
{
    if (!wifi_mgmr_ap_state_get()) {
        return 0;
    }

    struct ifreq request = {};
    const int name_length = fhost_vif_name(
        MGMR_VIF_AP,
        request.ifr_name,
        sizeof(request.ifr_name));
    if (name_length <= 0 || name_length >= static_cast<int>(sizeof(request.ifr_name))) {
        errno = ENODEV;
        return -1;
    }

    return setsockopt(
        listener,
        SOL_SOCKET,
        SO_BINDTODEVICE,
        &request,
        sizeof(request));
}

bool create_listener_socket(void *, uint16_t port, int *handle)
{
    if (handle == nullptr || port == 0U) {
        return false;
    }
    tcp_runtime_diagnostic_begin_listener(port);
    const int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    tcp_runtime_diagnostic_note_listener_stage(
        TcpListenerDiagnosticStage::Socket,
        listener < 0 ? -1 : 0,
        listener < 0 ? errno : 0);
    if (listener < 0) {
        return false;
    }

    int reuse_address = 1;
    const int reuse_result = setsockopt(
        listener,
        SOL_SOCKET,
        SO_REUSEADDR,
        &reuse_address,
        sizeof(reuse_address));
    tcp_runtime_diagnostic_note_listener_stage(
        TcpListenerDiagnosticStage::ReuseAddress,
        reuse_result,
        reuse_result == 0 ? 0 : errno);
    const bool nonblocking = set_nonblocking(listener, true);
    tcp_runtime_diagnostic_note_listener_stage(
        TcpListenerDiagnosticStage::Nonblocking,
        nonblocking ? 0 : -1,
        nonblocking ? 0 : errno);
    if (reuse_result != 0 || !nonblocking) {
        close_listener_setup_failure(listener);
        return false;
    }

    struct sockaddr_in local = {};
    local.sin_len = sizeof(local);
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = listener_bind_address();
    const uint32_t configured_local_address = local.sin_addr.s_addr;
    const int bind_result = bind(
        listener,
        reinterpret_cast<struct sockaddr *>(&local),
        sizeof(local));
    tcp_runtime_diagnostic_note_listener_stage(
        TcpListenerDiagnosticStage::Bind,
        bind_result,
        bind_result == 0 ? 0 : errno);
    if (bind_result != 0) {
        close_listener_setup_failure(listener);
        return false;
    }

    struct sockaddr_in observed_local = {};
    socklen_t observed_length = sizeof(observed_local);
    const int observed_result = getsockname(
        listener,
        reinterpret_cast<struct sockaddr *>(&observed_local),
        &observed_length);
    tcp_runtime_diagnostic_note_listener_address(
        configured_local_address,
        observed_result == 0 ? observed_local.sin_addr.s_addr : 0U,
        observed_result == 0 ? ntohs(observed_local.sin_port) : 0U,
        observed_result == 0 ? 0 : errno);
    const int listen_result = listen(listener, 5);
    tcp_runtime_diagnostic_note_listener_stage(
        TcpListenerDiagnosticStage::Listen,
        listen_result,
        listen_result == 0 ? 0 : errno);
    if (listen_result != 0) {
        close_listener_setup_failure(listener);
        return false;
    }

#if BL616CL_APSTA_LISTENER_BIND_DEVICE
    /*
     * lwIP replaces the bound TCP PCB with a listen PCB and clears its
     * netif_idx. Apply SO_BINDTODEVICE only after listen() so AP+STA traffic
     * remains associated with the AP input interface.
     */
    const int bind_device_result = bind_listener_to_active_ap_interface(listener);
    tcp_runtime_diagnostic_note_listener_stage(
        TcpListenerDiagnosticStage::BindToDevice,
        bind_device_result,
        bind_device_result == 0 ? 0 : errno);
    if (bind_device_result != 0) {
        close_listener_setup_failure(listener);
        return false;
    }
#endif

    *handle = listener;
    tcp_runtime_diagnostic_note_listener_ready(listener);
    return true;
}

bool accept_socket(void *, int listener_handle, int *client_handle)
{
    if (client_handle == nullptr) {
        return false;
    }
    *client_handle = -1;
    const int accepted = accept(listener_handle, nullptr, nullptr);
    tcp_runtime_diagnostic_note_accept(accepted, accepted < 0 ? errno : 0);
    if (accepted < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }
    forget_closed_handle(accepted);
    struct sockaddr_storage peer = {};
    socklen_t peer_length = sizeof(peer);
    const int peer_result = getpeername(
        accepted,
        reinterpret_cast<struct sockaddr *>(&peer),
        &peer_length);
    const int peer_errno = peer_result == 0 ? 0 : errno;
    int socket_error = 0;
    socklen_t socket_error_length = sizeof(socket_error);
    const int socket_result = getsockopt(
        accepted,
        SOL_SOCKET,
        SO_ERROR,
        &socket_error,
        &socket_error_length);
    const int socket_errno = socket_result == 0 ? 0 : errno;
    tcp_runtime_diagnostic_note_accepted_socket(
        accepted,
        peer_result,
        peer_errno,
        socket_result,
        socket_errno,
        socket_error);
    /*
     * lwIP accepts this connection from an already nonblocking listener.
     * Do not reconfigure the accepted descriptor here: this SDK profile can
     * reject ioctlsocket immediately after accept and then drop a completed
     * TCP handshake. The later read/write paths already request
     * nonblocking mode before they need it.
     */
    *client_handle = accepted;
    return true;
}

bool resolve_hostname(void *, const char *hostname, uint8_t address[4])
{
    if (hostname == nullptr || address == nullptr) {
        return false;
    }
    hostname_diagnostic = {};
    hostname_diagnostic.flags = 1U;
    const ip_addr_t *server = dns_getserver(0U);
    if (server != nullptr && IP_IS_V4(server)) {
        const ip4_addr_t *server_v4 = ip_2_ip4(server);
        hostname_diagnostic.server_address[0] = ip4_addr1(server_v4);
        hostname_diagnostic.server_address[1] = ip4_addr2(server_v4);
        hostname_diagnostic.server_address[2] = ip4_addr3(server_v4);
        hostname_diagnostic.server_address[3] = ip4_addr4(server_v4);
    }
    ip_addr_t resolved = {};
    const err_t raw_error = netconn_gethostbyname(hostname, &resolved);
    hostname_diagnostic.raw_error = static_cast<int16_t>(raw_error);
    if (raw_error != ERR_OK || !IP_IS_V4(&resolved)) {
        hostname_diagnostic.h_error = static_cast<int16_t>(h_errno);
        return false;
    }
    const ip4_addr_t *resolved_v4 = ip_2_ip4(&resolved);
    address[0] = ip4_addr1(resolved_v4);
    address[1] = ip4_addr2(resolved_v4);
    address[2] = ip4_addr3(resolved_v4);
    address[3] = ip4_addr4(resolved_v4);
    hostname_diagnostic.flags |= 2U;
    return true;
}

bool get_hostname_diagnostic(void *, TcpHostnameDiagnostic *diagnostic)
{
    if (diagnostic == nullptr) {
        return false;
    }
    *diagnostic = hostname_diagnostic;
    return true;
}

bool connect_socket(
    void *,
    int handle,
    const uint8_t address[4],
    uint16_t port,
    uint32_t timeout_ms)
{
    if (address == nullptr || port == 0U || timeout_ms == 0U) {
        return false;
    }
    struct sockaddr_in remote = {};
    remote.sin_len = sizeof(remote);
    remote.sin_family = AF_INET;
    remote.sin_port = htons(port);
    remote.sin_addr.s_addr = htonl(
        (static_cast<uint32_t>(address[0]) << 24U) |
        (static_cast<uint32_t>(address[1]) << 16U) |
        (static_cast<uint32_t>(address[2]) << 8U) |
        static_cast<uint32_t>(address[3]));

    /*
     * This SDK profile can report a nonblocking socket writable with SO_ERROR
     * clear while the connection is still only queued locally. A Windows peer
     * then sees no SYN even though WiFiClient::connect() returned success.
     * Use bounded blocking connect so lwIP does not return success until its
     * connected/error callback has completed the TCP handshake.
     */
    if (!set_socket_timeouts(handle, timeout_ms)) {
        return false;
    }
    return connect(
        handle,
        reinterpret_cast<struct sockaddr *>(&remote),
        sizeof(remote)) == 0;
}

bool send_socket(
    void *,
    int handle,
    const uint8_t *data,
    size_t size,
    size_t *sent)
{
    if (data == nullptr || sent == nullptr) {
        return false;
    }

    /*
     * A TCP send only reports that lwIP accepted bytes into its local queue.
     * Probe the receive/error side first so an already delivered RST or FIN is
     * returned to WiFiS3 as CLIENTSEND=ERROR instead of a false full write.
     */
    if (probe_socket_peer(handle) != SocketPeerState::Alive) {
        *sent = 0U;
        return false;
    }

    *sent = 0U;
    bool completed = false;
    const unsigned long started_at = millis();
    while (*sent < size) {
        const int result = send(handle, data + *sent, size - *sent, 0);
        if (result > 0) {
            *sent += static_cast<size_t>(result);
            continue;
        }
        if (result == 0) {
            break;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }

        const unsigned long elapsed = millis() - started_at;
        if (elapsed >= kSocketIoTimeoutMs ||
            !wait_writable(handle, kSocketIoTimeoutMs - elapsed)) {
            break;
        }
    }
    completed = *sent == size;

    return completed;
}

bool receive_socket(
    void *,
    int handle,
    uint8_t *data,
    size_t capacity,
    size_t *received)
{
    if (data == nullptr || received == nullptr) {
        return false;
    }
    if (!set_nonblocking(handle, true)) {
        return false;
    }
    const int result = recv(handle, data, capacity, 0);
    const int socket_errno = errno;
    if (result < 0) {
        if (socket_errno == EWOULDBLOCK || socket_errno == EAGAIN) {
            *received = 0U;
            return true;
        }
        return false;
    }
    *received = static_cast<size_t>(result);
    return true;
}

bool available_socket(void *, int handle, size_t *bytes)
{
    if (bytes == nullptr) {
        return false;
    }
    const SocketPeerState peer_state = probe_socket_peer(handle);
    if (peer_state == SocketPeerState::ProbeFailure) {
        return false;
    }
    if (peer_state == SocketPeerState::Closed) {
        *bytes = 0U;
        return true;
    }
    /*
     * The bridge profile intentionally has LWIP_SO_RCVBUF disabled, so
     * FIONREAD is not implemented by lwip_ioctl. Probe one byte without
     * consuming it instead; return 1 because WiFiS3 only uses this value as
     * a readiness test before issuing CLIENTRECEIVE.
     */
    if (!set_nonblocking(handle, true)) {
        return false;
    }
    uint8_t byte = 0U;
    const int result = recv(handle, &byte, 1U, MSG_PEEK);
    const int socket_errno = errno;
    tcp_runtime_diagnostic_note_available(result, socket_errno);
    if (result > 0) {
        *bytes = 1U;
        return true;
    }
    if (result == 0 ||
        socket_errno == EWOULDBLOCK ||
        socket_errno == EAGAIN) {
        *bytes = 0U;
        return true;
    }
    return false;
}

bool peek_socket(void *, int handle, int *value)
{
    if (value == nullptr) {
        return false;
    }
    if (!set_nonblocking(handle, true)) {
        return false;
    }
    uint8_t byte = 0U;
    const int result = recv(handle, &byte, 1U, MSG_PEEK);
    const int socket_errno = errno;
    if (result == 0) {
        *value = -1;
        return true;
    }
    if (result < 0) {
        if (socket_errno == EWOULDBLOCK || socket_errno == EAGAIN) {
            *value = -1;
            return true;
        }
        return false;
    }
    *value = byte;
    return true;
}

bool connected_socket(void *, int handle, bool *connected)
{
    if (connected == nullptr) {
        return false;
    }
    struct sockaddr_storage peer = {};
    socklen_t length = sizeof(peer);
    const bool has_peer = getpeername(
        handle,
        reinterpret_cast<struct sockaddr *>(&peer),
        &length) == 0;
    if (!has_peer) {
        *connected = false;
        return true;
    }
    const SocketPeerState peer_state = probe_socket_peer(handle);
    if (peer_state == SocketPeerState::ProbeFailure) {
        return false;
    }
    *connected = peer_state == SocketPeerState::Alive;
    return true;
}

bool get_state_socket(
    void *,
    int handle,
    uint8_t remote_address[4],
    uint16_t *remote_port,
    uint16_t *local_port)
{
    if (remote_address == nullptr || remote_port == nullptr || local_port == nullptr) {
        return false;
    }
    struct sockaddr_in remote = {};
    socklen_t remote_length = sizeof(remote);
    struct sockaddr_in local = {};
    socklen_t local_length = sizeof(local);
    if (getpeername(
            handle,
            reinterpret_cast<struct sockaddr *>(&remote),
            &remote_length) != 0 ||
        getsockname(
            handle,
            reinterpret_cast<struct sockaddr *>(&local),
            &local_length) != 0) {
        return false;
    }
    const uint32_t remote_ip = ntohl(remote.sin_addr.s_addr);
    remote_address[0] = static_cast<uint8_t>(remote_ip >> 24U);
    remote_address[1] = static_cast<uint8_t>(remote_ip >> 16U);
    remote_address[2] = static_cast<uint8_t>(remote_ip >> 8U);
    remote_address[3] = static_cast<uint8_t>(remote_ip);
    *remote_port = ntohs(remote.sin_port);
    *local_port = ntohs(local.sin_port);
    return true;
}

bool flush_socket(void *, int)
{
    return true;
}

bool get_local_address_socket(void *, int handle, uint8_t local_address[4])
{
    if (local_address == nullptr) {
        return false;
    }
    struct sockaddr_in local = {};
    socklen_t length = sizeof(local);
    if (getsockname(
            handle,
            reinterpret_cast<struct sockaddr *>(&local),
            &length) != 0) {
        return false;
    }
    const uint32_t ip = ntohl(local.sin_addr.s_addr);
    local_address[0] = static_cast<uint8_t>(ip >> 24U);
    local_address[1] = static_cast<uint8_t>(ip >> 16U);
    local_address[2] = static_cast<uint8_t>(ip >> 8U);
    local_address[3] = static_cast<uint8_t>(ip);
    return true;
}

#if BL616CL_TCP_RX_BUDGET_DIAGNOSTIC
bool get_rx_budget_diagnostic(void *, TcpRxBudgetDiagnostic *value)
{
    if (value == nullptr) return false;
#if !(LWIP_STATS && MEM_STATS && MEMP_STATS)
    return false;
#else
    using D = TcpRxBudgetDiagnostic;
    *value = D();
    auto &v = value->values;
    LOCK_TCPIP_CORE();
#if BL616CL_TCP_RX_FAIR_WINDOW
    if (!tcp_rx_native_policy_healthy()) { UNLOCK_TCPIP_CORE(); return false; }
#endif
    v[D::PoolCapacity] = MEMP_NUM_TCP_PCB;
    v[D::PoolUsed] = lwip_stats.memp[MEMP_TCP_PCB]->used;
    for (const tcp_pcb *p = tcp_active_pcbs; p != nullptr; p = p->next) {
        const uint32_t cap = TCP_WND_MAX(p);
        // Only positive sequence distance still promised to the peer counts.
        // uint32 subtraction handles a wrapped right edge. All receive windows
        // are below 2^31, as required by TCP sequence comparisons.
        const uint32_t distance = p->rcv_ann_right_edge - p->rcv_nxt;
        const uint32_t promised = static_cast<int32_t>(distance) > 0 ? distance : 0U;
        ++v[D::Active];
        if (p->state == ESTABLISHED) ++v[D::Established];
        else ++v[D::OtherActive];
        v[D::CapTotal] += cap;
        v[D::AcceptTotal] += p->rcv_wnd;
        if (p->rcv_wnd <= cap) v[D::WindowDebitTotal] += cap - p->rcv_wnd;
        else { UNLOCK_TCPIP_CORE(); return false; }
        v[D::AnnouncedOutstandingTotal] += promised;
        if (promised > p->rcv_wnd) ++v[D::AnnouncedOverAccept];
        if (v[D::Active] == 1U || p->rcv_wnd < v[D::MinAccept]) v[D::MinAccept] = p->rcv_wnd;
        if (p->rcv_wnd > v[D::MaxAccept]) v[D::MaxAccept] = p->rcv_wnd;
        if (promised > v[D::MaxAnnouncedOutstanding]) v[D::MaxAnnouncedOutstanding] = promised;
        if (p->rcv_wnd == 0U) ++v[D::ZeroWindows];
#if TCP_QUEUE_OOSEQ
        for (const tcp_seg *seg = p->ooseq; seg != nullptr; seg = seg->next) {
            ++v[D::OoseqSegments];
            v[D::OoseqPbufs] += pbuf_clen(seg->p);
            v[D::OoseqBytes] += seg->p->tot_len;
        }
#endif
        if (p->refused_data != nullptr) {
            v[D::RefusedPbufs] += pbuf_clen(p->refused_data);
            v[D::RefusedBytes] += p->refused_data->tot_len;
        }
    }
    for (const tcp_pcb *p = tcp_bound_pcbs; p != nullptr; p = p->next) ++v[D::Bound];
    for (const tcp_pcb *p = tcp_tw_pcbs; p != nullptr; p = p->next) ++v[D::TimeWait];
    v[D::HeapCapacity] = MEM_SIZE;
    v[D::HeapUsed] = lwip_stats.mem.used;
    v[D::HeapPeak] = lwip_stats.mem.max;
    v[D::HeapErrors] = lwip_stats.mem.err;
    UNLOCK_TCPIP_CORE();
    return true;
#endif
}
#endif

} // namespace

const TcpClientBackend &bouffalo_tcp_client_backend()
{
    static const TcpClientBackend backend = {
        nullptr,
        create_socket,
        close_socket,
        create_listener_socket,
        accept_socket,
        resolve_hostname,
        get_hostname_diagnostic,
        connect_socket,
        send_socket,
        receive_socket,
        available_socket,
        peek_socket,
        connected_socket,
        get_state_socket,
        flush_socket,
        get_local_address_socket,
#if BL616CL_TCP_RX_BUDGET_DIAGNOSTIC
        get_rx_budget_diagnostic,
#endif
    };
    return backend;
}

} // namespace bridge
