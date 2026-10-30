#pragma once
#include <stdint.h>
#ifndef BL616CL_TLS_FLOW_DIAGNOSTIC
#define BL616CL_TLS_FLOW_DIAGNOSTIC 0
#endif
#if BL616CL_TLS_FLOW_DIAGNOSTIC
extern "C" {
#include <lwip/opt.h>
}
#endif
namespace bridge {
#if BL616CL_TLS_FLOW_DIAGNOSTIC
// Internal SSLCLIENTHEAP suffix, layout 1. Caller holds the lwIP core lock.
// Addresses are matched internally and never included in the report.
struct TcpFlowDiagnostic {
    // layout, matched, local_port, state, rcv_nxt, rcv_wnd, rcv_ann_wnd,
    // rcv_ann_right_edge, lastack, snd_nxt, ooseq_bytes, refused_bytes,
    // flags, nrtx, rtime (signed int32 bits), TLS readable, peeked, tuple_valid.
    uint32_t values[18] = {1};
};
template<class Pcb, class Match>
void capture_tcp_flow(const Pcb *const *heads, unsigned count, Match match,
                      TcpFlowDiagnostic &result) {
    for (unsigned i = 1; i < 15; ++i) result.values[i] = 0;
    for (unsigned i = 0; i < count; ++i) {
        for (const Pcb *p = heads[i]; p; p = p->next) {
            if (!match(*p)) continue;
            auto &v = result.values;
            v[1] = 1; v[2] = p->local_port; v[3] = p->state;
            v[4] = p->rcv_nxt; v[5] = p->rcv_wnd;
            v[6] = p->rcv_ann_wnd; v[7] = p->rcv_ann_right_edge;
            v[8] = p->lastack; v[9] = p->snd_nxt;
#if TCP_QUEUE_OOSEQ
            for (const auto *seg = p->ooseq; seg; seg = seg->next)
                v[10] += seg->p->tot_len;
#endif
            v[11] = p->refused_data ? p->refused_data->tot_len : 0;
            v[12] = p->flags; v[13] = p->nrtx;
            v[14] = static_cast<uint32_t>(static_cast<int32_t>(p->rtime));
            return;
        }
    }
}
#endif
}
