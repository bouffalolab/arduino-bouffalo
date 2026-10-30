#ifndef UNOR4_BL616CL_STA_TCP_DIAGNOSTIC_H
#define UNOR4_BL616CL_STA_TCP_DIAGNOSTIC_H
#include <stddef.h>
#include <stdint.h>
#ifndef BL616CL_STA_TCP_DIAGNOSTIC
#define BL616CL_STA_TCP_DIAGNOSTIC 0
#endif
#ifndef BL616CL_STA_TCP_DIAGNOSTIC_PEER_PORT
#define BL616CL_STA_TCP_DIAGNOSTIC_PEER_PORT 53
#endif
static_assert(BL616CL_STA_TCP_DIAGNOSTIC_PEER_PORT > 0 &&
    BL616CL_STA_TCP_DIAGNOSTIC_PEER_PORT <= 65535, "Invalid passive TCP peer port");
namespace bridge {
// Only headers are retained. One outgoing STA connection to the configured port is selected
// by SYN; addresses are used internally to exclude unrelated connections.
struct StaTcpDiagnostic {
    uint32_t generation = 0, local_port = 0, rx_packets = 0, rx_bytes = 0;
    uint32_t last_seq = 0, highest_end = 0, last_length = 0, tx_acks = 0;
    uint32_t last_ack = 0, repeated_acks = 0, rx_flags = 0, tx_flags = 0;
    uint32_t last_window = 0, invalid_frames = 0;
    struct Event { uint32_t seq = 0, ack = 0; uint16_t length = 0; uint8_t flags = 0, output = 0; };
    Event events[4] = {};
    uint32_t event_count = 0, local_address = 0, peer_address = 0, syn_sequence = 0;
    uint32_t released = 0, acknowledged = 0, discarded = 0;
    uint32_t released_ack = 0, acknowledged_ack = 0, discarded_ack = 0;
    static uint16_t be16(const uint8_t *p) { return uint16_t(p[0]) * 256U + p[1]; }
    static uint32_t be32(const uint8_t *p) { return (uint32_t(be16(p)) << 16U) | be16(p + 2); }
    // release bits: 0 = ordinary header event; 1 = release, 2 = MAC ACK, 4 = discard.
    template<class Read> void observe(bool output, size_t packet_size, Read read, unsigned release = 0) {
        uint8_t eth[14], ip[20], tcp[20];
        if (packet_size < 54 || read(eth, 14, 0) != 14 || be16(eth + 12) != 0x800 ||
            read(ip, 20, 14) != 20 || (ip[0] >> 4) != 4 || ip[9] != 6) return;
        const size_t ihl = (ip[0] & 15U) * 4U, total = be16(ip + 2);
        // Fragmented TCP requires IP reassembly and cannot be decoded here.
        if (ihl < 20 || (be16(ip + 6) & 0x3fffU) || total < ihl + 20 ||
            total > packet_size - 14 || read(tcp, 20, 14 + ihl) != 20) {
            ++invalid_frames; return;
        }
        const size_t thl = (tcp[12] >> 4) * 4U;
        if (thl < 20 || total < ihl + thl) { ++invalid_frames; return; }
        const uint16_t source = be16(tcp), destination = be16(tcp + 2);
        const uint32_t src = be32(ip + 12), dst = be32(ip + 16);
        const uint8_t flags = tcp[13];
        if (!release && output && destination == BL616CL_STA_TCP_DIAGNOSTIC_PEER_PORT && (flags & 0x12U) == 2U) {
            // SYN retransmission belongs to the same generation.
            if (!generation || local_port != source || local_address != src || peer_address != dst ||
                syn_sequence != be32(tcp + 4)) {
                const uint32_t next = generation + 1U;
                *this = StaTcpDiagnostic(); generation = next ? next : 1;
                local_port = source; local_address = src; peer_address = dst;
                syn_sequence = be32(tcp + 4);
            }
        }
        if (!generation || (output
            ? source != local_port || destination != BL616CL_STA_TCP_DIAGNOSTIC_PEER_PORT || src != local_address || dst != peer_address
            : destination != local_port || source != BL616CL_STA_TCP_DIAGNOSTIC_PEER_PORT || dst != local_address || src != peer_address)) return;
        const uint32_t seq = be32(tcp + 4), ack = be32(tcp + 8);
        if (release) {
            // Completion may arrive after a new SYN selected another flow;
            // the four-tuple check above excludes those stale completions.
            if (!output || !(flags & 16U)) return;
            ++released; released_ack = ack;
            if (release & 2U) { ++acknowledged; acknowledged_ack = ack; }
            if (release & 4U) { ++discarded; discarded_ack = ack; }
            return;
        }
        const uint16_t length = static_cast<uint16_t>(total - ihl - thl);
        Event &event = events[event_count++ % 4U];
        event.seq = seq; event.ack = ack; event.length = length; event.flags = flags; event.output = output;
        if (output) {
            tx_flags = flags; last_window = be16(tcp + 14);
            if (flags & 16U) { if (tx_acks && ack == last_ack) ++repeated_acks; ++tx_acks; last_ack = ack; }
        } else {
            const uint32_t end = seq + length + ((flags & 2U) ? 1U : 0U) + ((flags & 1U) ? 1U : 0U);
            if (!rx_packets || static_cast<int32_t>(end - highest_end) > 0) highest_end = end;
            ++rx_packets; rx_bytes += length; last_seq = seq; last_length = length; rx_flags = flags;
        }
    }
};
void sta_tcp_observe(bool is_sta, bool output, void *packet);
void sta_tcp_tx_release(void *packet, uint32_t acknowledged, uint32_t discarded);
void sta_tcp_diagnostic_report(uint8_t page, uint8_t *report);
}
#endif
