#include "sta_tcp_diagnostic.h"
#if BL616CL_STA_TCP_DIAGNOSTIC
extern "C" {
#include <lwip/pbuf.h>
#include "sdk/mm/mm.h"
}
#endif
namespace bridge {
#if BL616CL_STA_TCP_DIAGNOSTIC
namespace { StaTcpDiagnostic diagnostic; }
#endif
void sta_tcp_observe(bool is_sta, bool output, void *packet) {
#if BL616CL_STA_TCP_DIAGNOSTIC
    if (!is_sta || !packet) return;
    auto *p = static_cast<struct pbuf *>(packet);
    // Header parsing is bounded (54 bytes), no allocation, payload or logging.
    const uintptr_t flags = mm_lock_save();
    diagnostic.observe(output, p->tot_len, [p](uint8_t *out, size_t n, size_t offset) {
        return static_cast<size_t>(pbuf_copy_partial(p, out, n, offset));
    });
    mm_unlock_restore(flags);
#else
    (void)is_sta; (void)output; (void)packet;
#endif
}
void sta_tcp_diagnostic_report(uint8_t page, uint8_t *report) {
    report[5] = page; report[6] = BL616CL_STA_TCP_DIAGNOSTIC ? 1 : 0;
    if (page == 58) report[7] = 1;
#if BL616CL_STA_TCP_DIAGNOSTIC
    const auto put = [report](size_t offset, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) report[offset + i] = static_cast<uint8_t>(value >> (8U * i));
    };
    // Preserve interrupt state in both network tasks and USB control callbacks.
    const uintptr_t flags = mm_lock_save();
    if (page == 46) {
        const uint32_t values[] = {diagnostic.generation, diagnostic.local_port, diagnostic.rx_packets,
            diagnostic.rx_bytes, diagnostic.last_seq, diagnostic.highest_end, diagnostic.last_length,
            diagnostic.tx_acks, diagnostic.last_ack, diagnostic.repeated_acks, diagnostic.rx_flags,
            diagnostic.tx_flags, diagnostic.last_window, diagnostic.invalid_frames};
        for (unsigned i = 0; i < 14; ++i) put(8 + 4 * i, values[i]);
    } else if (page == 58) {
        const uint32_t values[] = {diagnostic.generation, diagnostic.local_port,
            diagnostic.released, diagnostic.acknowledged, diagnostic.discarded,
            diagnostic.released_ack, diagnostic.acknowledged_ack, diagnostic.discarded_ack,
            BL616CL_STA_TCP_DIAGNOSTIC_PEER_PORT};
        for (unsigned i = 0; i < 9; ++i) put(8 + 4 * i, values[i]);
    } else if (page == 47) {
        put(8, diagnostic.generation); put(12, diagnostic.event_count);
        for (unsigned i = 0; i < 4; ++i) {
            const auto &event = diagnostic.events[(diagnostic.event_count + i) % 4];
            const size_t offset = 16 + 12 * i;
            put(offset, event.seq); put(offset + 4, event.ack);
            report[offset + 8] = event.length; report[offset + 9] = event.length >> 8;
            report[offset + 10] = event.flags; report[offset + 11] = event.output;
        }
    }
    mm_unlock_restore(flags);
#endif
}
void sta_tcp_tx_release(void *packet, uint32_t acknowledged, uint32_t discarded) {
#if BL616CL_STA_TCP_DIAGNOSTIC
    if (!packet) return;
    auto *p = static_cast<struct pbuf *>(packet);
    // FHOST may have revealed its reserved headroom before this pre-free hook.
    // Read only Ethernet/IP headers to find the native packet offset.
    const uint16_t offsets[] = {0, PBUF_LINK_ENCAPSULATION_HLEN};
    for (const uint16_t base : offsets) {
        uint8_t header[34];
        if (base > p->tot_len ||
            pbuf_copy_partial(p, header, sizeof(header), base) != sizeof(header) ||
            header[12] != 8 || header[13] != 0 || (header[14] >> 4) != 4 || header[23] != 6) continue;
        const uintptr_t flags = mm_lock_save();
        diagnostic.observe(true, p->tot_len - base, [p, base](uint8_t *out, size_t n, size_t offset) {
            return static_cast<size_t>(pbuf_copy_partial(p, out, n, base + offset));
        }, 1U | (acknowledged ? 2U : 0U) | (discarded ? 4U : 0U));
        mm_unlock_restore(flags);
        break;
    }
#else
    (void)packet; (void)acknowledged; (void)discarded;
#endif
}
}
