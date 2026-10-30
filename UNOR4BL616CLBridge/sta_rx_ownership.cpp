#include "sta_rx_ownership.h"
#include "tcp_receive_window.h"
#include "tcp_tx_preallocation.h"
#include "tcp_rx_native_policy.h"
#include "sta_tcp_diagnostic.h"
#if BL616CL_TCP_RX_ATOMIC_RESERVE
static_assert(BL616CL_STA_TCP_RX_CHUNK_BYTES && BL616CL_STA_TCP_RX_RESERVE_BYTES,
    "Atomic RX reserve requires chunked owned RX and a nonzero reserve");
#endif
#if BL616CL_STA_TCP_RX_CHUNK_BYTES
static_assert(BL616CL_STA_TCP_OWNED_RX || BL616CL_AP_TCP_OWNED_RX, "RX chunks require owned RX");
static_assert(BL616CL_STA_TCP_RX_CHUNK_BYTES >= 256 && BL616CL_STA_TCP_RX_CHUNK_BYTES <= 1460,
    "RX chunk size must retain complete Ethernet/IP/TCP headers in its first pbuf");
#endif
#if BL616CL_STA_TCP_RX_RESERVE_BYTES
static_assert(BL616CL_STA_TCP_OWNED_RX || BL616CL_AP_TCP_OWNED_RX, "RX reserve requires owned RX");
extern "C" {
#include <lwip/opt.h>
#include <lwip/stats.h>
}
static_assert(BL616CL_STA_TCP_RX_RESERVE_BYTES < MEM_SIZE, "RX reserve exceeds lwIP heap");
#endif
#if BL616CL_TCP_OOSEQ_GLOBAL_PBUF_LIMIT
static_assert(BL616CL_TCP_OOSEQ_PBUF_LIMIT > 0, "Global OOSEQ limit requires the per-PCB limit");
static_assert(BL616CL_TCP_OOSEQ_GLOBAL_PBUF_LIMIT <= 65535, "Invalid global OOSEQ pbuf budget");
extern "C" {
#include <lwip/priv/tcp_priv.h>
}
#endif
#if BL616CL_TCP_OOSEQ_PBUF_LIMIT
static_assert(BL616CL_TCP_OOSEQ_PBUF_LIMIT <= 65535, "Invalid TCP OOSEQ pbuf budget");
extern "C" unsigned bridge_tcp_ooseq_pbuf_limit(void) {
#if BL616CL_TCP_OOSEQ_GLOBAL_PBUF_LIMIT
    // lwIP invokes this only on its TCP/IP thread, after inserting into OOSEQ.
    // If all active PCBs together exceed the budget, native tcp_in.c drops the
    // current PCB's queue and sends its normal ACK. Never touch another PCB or
    // its pbuf ownership here. In-order/netconn queues remain outside this cap;
    // this is not a guarantee of free lwIP heap or a fairness policy.
    unsigned total = 0;
    for (const struct tcp_pcb *pcb = tcp_active_pcbs; pcb; pcb = pcb->next) {
        for (const struct tcp_seg *seg = pcb->ooseq; seg; seg = seg->next) {
            total += pbuf_clen(seg->p);
            if (total > BL616CL_TCP_OOSEQ_GLOBAL_PBUF_LIMIT) return 0;
        }
    }
#endif
    return BL616CL_TCP_OOSEQ_PBUF_LIMIT;
}
#endif
#if BL616CL_STA_TCP_OWNED_RX || BL616CL_AP_TCP_OWNED_RX
extern "C" {
#include <lwip/pbuf.h>
#include "sdk/mm/mm.h"
}
#if BL616CL_TCP_RX_ATOMIC_RESERVE
extern "C" struct pbuf *bridge_pbuf_alloc_reserved(uint16_t, unsigned, int *);
#endif
namespace {
uint32_t copied, failed, bytes;
uint32_t udp_fragments, udp_copied, udp_dropped;
uint16_t udp_id, udp_count, udp_offsets[12];
#if BL616CL_STA_TCP_RX_CHUNK_BYTES
struct pbuf *clone_in_chunks(const struct pbuf *source, bool payload, bool &reserve_drop, bool whole_frame = false) {
    struct pbuf *head = nullptr;
    unsigned offset = 0;
    while (offset < source->tot_len) {
        const unsigned remaining = source->tot_len - offset;
        const unsigned length = whole_frame || remaining < BL616CL_STA_TCP_RX_CHUNK_BYTES
            ? remaining : BL616CL_STA_TCP_RX_CHUNK_BYTES;
#if BL616CL_TCP_RX_ATOMIC_RESERVE
        int denied = 0;
        struct pbuf *part = payload ? bridge_pbuf_alloc_reserved(length,
            BL616CL_STA_TCP_RX_RESERVE_BYTES, &denied) : pbuf_alloc(PBUF_RAW, length, PBUF_RAM);
        if (denied) reserve_drop = true;
#else
        (void)payload; (void)reserve_drop;
        struct pbuf *part = pbuf_alloc(PBUF_RAW, length, PBUF_RAM);
#endif
        if (!part) {
            if (head) pbuf_free(head);
            return nullptr;
        }
        if (pbuf_copy_partial(source, part->payload, length, offset) != length) {
            pbuf_free(part);
            if (head) pbuf_free(head);
            return nullptr;
        }
        // pbuf_cat transfers our existing reference; pbuf_chain would retain it.
        if (head) pbuf_cat(head, part);
        else head = part;
        offset += length;
    }
    return head;
}
#endif
#if BL616CL_STA_TCP_RX_RESERVE_BYTES
uint32_t reserve_dropped;
#if BL616CL_TCP_RX_MEMORY_OWNER
bool ipv6_has_payload(const struct pbuf *p) {
    uint8_t ip[40];
    if (pbuf_copy_partial(p, ip, sizeof(ip), 14) != sizeof(ip)) return true;
    unsigned remaining = unsigned(ip[4]) * 256U + ip[5];
    unsigned offset = 54, next = ip[6];
    for (unsigned n = 0; n < 8; ++n) {
        if (next == 6) {
            uint8_t tcp[20];
            if (remaining < sizeof(tcp) ||
                pbuf_copy_partial(p, tcp, sizeof(tcp), offset) != sizeof(tcp)) return true;
            const unsigned length = (tcp[12] >> 4) * 4U;
            return length < 20 || remaining != length;
        }
        uint8_t ext[8];
        if (remaining < sizeof(ext) ||
            pbuf_copy_partial(p, ext, sizeof(ext), offset) != sizeof(ext)) return true;
        unsigned length;
        if (next == 44) {
            if ((ext[2] & 0xffU) || (ext[3] & 0xf8U)) return true; // noninitial fragment
            length = 8;
        } else if (next == 0 || next == 43 || next == 60) {
            length = (unsigned(ext[1]) + 1U) * 8U;
        } else if (next == 51) {
            length = (unsigned(ext[1]) + 2U) * 4U;
        } else if (next == 58) {
            // Neighbor/router discovery must still enter the control reserve.
            return ext[0] < 133 || ext[0] > 137;
        } else return true;
        if (length > remaining) return true;
        remaining -= length; offset += length; next = ext[0];
    }
    return true; // malformed/overlong extension chains get no control exemption
}
#endif
bool tcp_has_payload(const struct pbuf *p, const uint8_t *ip) {
    const unsigned ihl = (ip[0] & 15U) * 4U;
    const unsigned total = unsigned(ip[2]) * 256U + ip[3];
    // Noninitial fragments and malformed headers receive no control exemption.
    if (ihl < 20U || (ip[6] & 31U) || ip[7]) return true;
    uint8_t tcp[20];
    if (pbuf_copy_partial(p, tcp, sizeof(tcp), 14U + ihl) != sizeof(tcp)) return true;
    const unsigned thl = (tcp[12] >> 4U) * 4U;
    if (thl < 20U || total < ihl + thl) return true;
    return total != ihl + thl;
}
#if !BL616CL_TCP_RX_ATOMIC_RESERVE
bool rx_copy_has_room(const struct pbuf *p) {
    const uint32_t limit = MEM_SIZE - BL616CL_STA_TCP_RX_RESERVE_BYTES;
    const uint32_t used = lwip_stats.mem.used;
    // Conservative estimate; the postallocation check also observes the actual
    // allocator charge. These are soft snapshots, not an atomic reservation.
#if BL616CL_STA_TCP_RX_CHUNK_BYTES
    const uint32_t blocks = (uint32_t(p->tot_len) + BL616CL_STA_TCP_RX_CHUNK_BYTES - 1U) /
        BL616CL_STA_TCP_RX_CHUNK_BYTES;
#else
    const uint32_t blocks = 1U;
#endif
    const uint32_t charge = uint32_t(p->tot_len) + blocks * (sizeof(struct pbuf) + 2U * MEM_ALIGNMENT + 32U);
    return used <= limit && charge <= limit - used;
}
#endif
#endif
}
#endif
namespace bridge {
void *sta_rx_detach(bool is_sta, void *packet) {
#if BL616CL_STA_TCP_OWNED_RX || BL616CL_AP_TCP_OWNED_RX
    auto *p = static_cast<struct pbuf *>(packet);
    uint8_t header[34];
    if (!(is_sta ? BL616CL_STA_TCP_OWNED_RX : BL616CL_AP_TCP_OWNED_RX) || !p ||
        pbuf_copy_partial(p, header, sizeof(header), 0) != sizeof(header))
        return packet;
    const bool ipv4_tcp = header[12] == 8 && header[13] == 0 &&
        (header[14] >> 4) == 4 && header[23] == 6;
    // Reassembly retains every IPv4 fragment. A 16 KiB UDP datagram needs
    // twelve radio frames, exceeding the scarce RX descriptors if retained
    // directly. Detach only fragmented UDP; ordinary UDP keeps its old path.
    const bool ipv4_udp_fragment = header[12] == 8 && header[13] == 0 &&
        (header[14] >> 4) == 4 && header[23] == 17 &&
        ((header[20] & 0x3fU) != 0 || header[21] != 0);
    // IPv6 extension/fragment headers can conceal TCP until native reassembly.
    // Copy the entire IPv6 frame in owner mode, without changing its bytes.
    const bool ipv6_owned = BL616CL_TCP_RX_MEMORY_OWNER && header[12] == 0x86 &&
        header[13] == 0xdd && (header[14] >> 4) == 6;
    if (!ipv4_tcp && !ipv6_owned && !ipv4_udp_fragment)
        return packet;
    // TCP may retain the input in OOSEQ or netconn queues. Release the scarce
    // radio RX reference before lwIP can retain it, including on allocation failure.
    // The full Ethernet frame (and any IP/TCP options/fragments) stays byte exact.
    struct pbuf *owned = nullptr;
    bool reserve_drop = false;
#if BL616CL_STA_TCP_RX_RESERVE_BYTES
    const bool payload =
#if BL616CL_TCP_RX_MEMORY_OWNER
        ipv6_owned ? ipv6_has_payload(p) :
#endif
        ipv4_udp_fragment || tcp_has_payload(p, header + 14);
#if !BL616CL_TCP_RX_ATOMIC_RESERVE
    reserve_drop = payload && !rx_copy_has_room(p);
#endif
#elif BL616CL_STA_TCP_RX_CHUNK_BYTES
    const bool payload = true;
#endif
    if (!reserve_drop) {
#if BL616CL_STA_TCP_RX_CHUNK_BYTES
        // UDP reassembly counts pbuf nodes, so retain one node per radio
        // fragment instead of splitting it into TCP allocation chunks.
        owned = clone_in_chunks(p, payload, reserve_drop, ipv4_udp_fragment);
#else
        owned = pbuf_clone(PBUF_RAW, PBUF_RAM, p);
#endif
    }
#if BL616CL_STA_TCP_RX_RESERVE_BYTES && !BL616CL_TCP_RX_ATOMIC_RESERVE
    if (payload && owned && lwip_stats.mem.used > MEM_SIZE - BL616CL_STA_TCP_RX_RESERVE_BYTES) {
        pbuf_free(owned);
        owned = nullptr;
        reserve_drop = true;
    }
#endif
    const uintptr_t flags = mm_lock_save();
    if (ipv4_udp_fragment) {
        const uint16_t id = uint16_t(header[18]) * 256U + header[19];
        if (!udp_fragments || id != udp_id) {
            udp_id = id; udp_count = 0;
            for (auto &offset : udp_offsets) offset = 0;
        }
        udp_offsets[udp_count % 12] = (uint16_t(header[20] & 31U) * 256U + header[21]) |
            (owned ? 0x8000U : 0x4000U);
        ++udp_count; ++udp_fragments;
        if (owned) ++udp_copied; else ++udp_dropped;
    }
    if (owned) { ++copied; bytes += p->tot_len; }
    else if (!reserve_drop) { ++failed; }
#if BL616CL_STA_TCP_RX_RESERVE_BYTES
    else { ++reserve_dropped; }
#endif
    mm_unlock_restore(flags);
    pbuf_free(p);
    return owned;
#else
    (void)is_sta;
    return packet;
#endif
}
void udp_rx_fragment_report(uint8_t *report) {
    report[5] = 81; report[6] = 1; report[7] = 1;
#if BL616CL_STA_TCP_OWNED_RX || BL616CL_AP_TCP_OWNED_RX
    const uintptr_t flags = mm_lock_save();
    const uint32_t values[] = {udp_fragments, udp_copied, udp_dropped};
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 4; ++j) report[8+4*i+j] = values[i] >> (8*j);
    report[20] = udp_id; report[21] = udp_id >> 8;
    report[22] = udp_count; report[23] = udp_count >> 8;
    for (unsigned i = 0; i < 12; ++i) {
        report[24+2*i] = udp_offsets[i]; report[25+2*i] = udp_offsets[i] >> 8;
    }
    mm_unlock_restore(flags);
#endif
}
void sta_rx_ownership_report(uint8_t *report) {
    for (unsigned j = 0; j < 4; ++j)
        report[32+j] = uint32_t(BL616CL_TCP_RECEIVE_WINDOW_BYTES) >> (8*j);
    for (unsigned j = 0; j < 4; ++j)
        report[36+j] = uint32_t(BL616CL_STA_TCP_RX_CHUNK_BYTES) >> (8*j);
    for (unsigned j = 0; j < 4; ++j)
        report[40+j] = uint32_t(BL616CL_TCP_TX_EXTRA_BYTES) >> (8*j);
    report[44] = 'T'; report[45] = 'X'; report[46] = 'P'; report[47] = '1';
    // Reserved extension: only new images identify the passive header filter.
    for (unsigned j = 0; j < 4; ++j)
        report[48+j] = uint32_t(BL616CL_STA_TCP_DIAGNOSTIC_PEER_PORT) >> (8*j);
    report[52] = 'T'; report[53] = 'C'; report[54] = 'P'; report[55] = '1';
    report[61] = BL616CL_TCP_RX_ATOMIC_RESERVE ? 1 : 0;
    report[56] = BL616CL_AP_TCP_OWNED_RX ? 1 : 0;
    report[57] = 'A'; report[58] = 'P'; report[59] = 'R'; report[60] = '1';
    report[5] = 51; report[6] = BL616CL_STA_TCP_OWNED_RX ? 1 : 0;
    report[7] = BL616CL_TCP_OOSEQ_PBUF_LIMIT > 255 ? 255 : BL616CL_TCP_OOSEQ_PBUF_LIMIT;
    for (unsigned j = 0; j < 4; ++j)
        report[20+j] = uint32_t(BL616CL_TCP_OOSEQ_GLOBAL_PBUF_LIMIT) >> (8*j);
    for (unsigned j = 0; j < 4; ++j)
        report[24+j] = uint32_t(BL616CL_STA_TCP_RX_RESERVE_BYTES) >> (8*j);
#if BL616CL_STA_TCP_OWNED_RX || BL616CL_AP_TCP_OWNED_RX
    const uintptr_t flags = mm_lock_save();
    const uint32_t values[] = {copied, failed, bytes};
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 4; ++j) report[8+4*i+j] = values[i] >> (8*j);
#if BL616CL_STA_TCP_RX_RESERVE_BYTES
    for (unsigned j = 0; j < 4; ++j) report[28+j] = reserve_dropped >> (8*j);
#endif
    mm_unlock_restore(flags);
#endif
}
}
