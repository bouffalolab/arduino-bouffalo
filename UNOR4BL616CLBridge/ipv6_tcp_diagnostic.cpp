#include "ipv6_tcp_diagnostic.h"
#if BL616CL_IPV6_TCP_DIAGNOSTIC
#include <Arduino.h>
extern "C" {
#include "lwip/sockets.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "fhost.h"
#include "wifi_mgmr_ext.h"
#include <string.h>
#include "lwip/errno.h"
#include "sdk/mm/mm.h"
}
namespace {
constexpr unsigned Port = 46160;
int listener = -1, peer = -1;
uint8_t pending[256];
unsigned size, offset;
uint32_t activity;
// Written only by the bridge loop. HID copies under an IRQ guard.
uint32_t state, connections, received, sent, closed, errors, last_error;
uint32_t rx6, tx6, rx_ns, tx_na, rx_syn;
uint8_t ap_address[16];
uint32_t ap_state, ap_checked;
bool would_block() { return errno == EWOULDBLOCK || errno == EAGAIN; }
bool nonblocking(int fd) {
    unsigned long enabled = 1;
    return lwip_ioctl(fd, FIONBIO, &enabled) == 0;
}
void error() { ++errors; last_error = errno; }
void close_peer() {
    if (peer >= 0) { lwip_close(peer); peer = -1; ++closed; }
    size = offset = 0;
    state = listener >= 0 ? 1 : 0;
}
}
#endif
namespace bridge {
void ipv6_tcp_diagnostic_poll() {
#if BL616CL_IPV6_TCP_DIAGNOSTIC
    const uint32_t now = millis();
    if (static_cast<uint32_t>(now - ap_checked) >= 250) {
        ap_checked = now;
        uint8_t address[16] = {}; uint32_t status = 0;
        LOCK_TCPIP_CORE();
        auto *ap = static_cast<netif *>(fhost_to_net_if(MGMR_VIF_AP));
        if (ap && netif_is_up(ap)) {
            if (ip6_addr_isinvalid(netif_ip6_addr_state(ap, 0)))
                netif_create_ip6_linklocal_address(ap, 1);
            status = netif_ip6_addr_state(ap, 0);
            memcpy(address, netif_ip6_addr(ap, 0)->addr, sizeof(address));
        }
        UNLOCK_TCPIP_CORE();
        const uintptr_t flags = mm_lock_save();
        memcpy(ap_address, address, sizeof(address)); ap_state = status;
        mm_unlock_restore(flags);
    }
    // Called only after the Wi-Fi/runtime initialization gate is ready.
    // One nonblocking receive/send per poll bounds work and buffering.
    if (listener < 0) {
        // A failed bind is visible and terminal until reset, not silently retried.
        if (state == 3) return;
        listener = lwip_socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        if (listener < 0) { error(); state = 3; return; }
        sockaddr_in6 address = {};
        address.sin6_family = AF_INET6; address.sin6_port = htons(Port);
        int only_v6 = 1;
        if (!nonblocking(listener) ||
            lwip_setsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY, &only_v6, sizeof(only_v6)) ||
            lwip_bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) ||
            lwip_listen(listener, 1)) {
            error(); lwip_close(listener); listener = -1; state = 3; return;
        }
        state = 1;
    }
    if (peer < 0) {
        peer = lwip_accept(listener, nullptr, nullptr);
        if (peer < 0) { if (!would_block()) error(); return; }
        if (!nonblocking(peer)) { error(); close_peer(); return; }
        ++connections; state = 2; activity = millis();
    }
    if (static_cast<uint32_t>(millis() - activity) >= 10000) { close_peer(); return; }
    if (offset == size) {
        const int count = lwip_recv(peer, pending, sizeof(pending), 0);
        if (!count) { close_peer(); return; }
        if (count < 0) { if (!would_block()) { error(); close_peer(); } return; }
        size = count; offset = 0; received += count; activity = millis();
    }
    const int count = lwip_send(peer, pending + offset, size - offset, 0);
    if (count < 0) { if (!would_block()) { error(); close_peer(); } return; }
    if (count > 0) { offset += count; sent += count; activity = millis(); }
#endif
}
void ipv6_tcp_diagnostic_observe(bool output, void *packet) {
#if BL616CL_IPV6_TCP_DIAGNOSTIC
    if (!packet) return;
    auto *p = static_cast<pbuf *>(packet);
    uint8_t header[68];
    const unsigned count = pbuf_copy_partial(p, header, sizeof(header), 0);
    if (count < 54 || header[12] != 0x86 || header[13] != 0xdd || (header[14] >> 4) != 6) return;
    const uintptr_t flags = mm_lock_save();
    if (output) ++tx6; else ++rx6;
    if (count >= 55 && header[20] == 58) {
        if (!output && header[54] == 135) ++rx_ns;
        if (output && header[54] == 136) ++tx_na;
    }
    if (!output && count >= 68 && header[20] == 6 && (header[67] & 2)) ++rx_syn;
    mm_unlock_restore(flags);
#else
    (void)output; (void)packet;
#endif
}
void ipv6_tcp_diagnostic_report(uint8_t *report) {
    const uint8_t page = report[5];
    report[5] = page == 62 ? 62 : 61; report[6] = BL616CL_IPV6_TCP_DIAGNOSTIC; report[7] = 1;
#if BL616CL_IPV6_TCP_DIAGNOSTIC
    const uintptr_t flags = mm_lock_save();
    if (page == 62) {
        memcpy(report + 8, ap_address, sizeof(ap_address));
        report[24] = ap_state;
        mm_unlock_restore(flags);
        return;
    }
    const uint32_t values[] = {state, Port, connections, received, sent, closed,
        errors, last_error, size - offset, rx6, tx6, rx_ns, tx_na, rx_syn};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        for (unsigned j = 0; j < 4; ++j) report[8 + 4*i + j] = values[i] >> (8*j);
    mm_unlock_restore(flags);
#endif
}
}
