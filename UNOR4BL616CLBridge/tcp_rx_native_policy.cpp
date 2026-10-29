#include "tcp_rx_native_policy.h"
#if BL616CL_TCP_RX_FAIR_WINDOW
#if !BL616CL_TCP_RX_ATOMIC_RESERVE || !BL616CL_STA_TCP_OWNED_RX || !BL616CL_AP_TCP_OWNED_RX
#error "Fair sequence windows require atomic owned RX admission for STA and AP"
#endif
#include "tcp_rx_credit_pool.h"
extern "C" {
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#if BL616CL_TCP_RX_MEMORY_OWNER
#include "sdk/mm/mm.h"
unsigned bridge_lwip_mem_charge(const void *);
#endif
}
namespace {
using bridge::TcpRxCreditPool;
// 32 future/current minima reserve 8192 B. The remaining 4096 B gives
// each of the eight concurrently busy TLS flows up to two extra quanta,
// without an early idle flow pinning a 2 KiB promise.
constexpr unsigned Minimum = 256, Cap = 768, Quantum = 256;
// This is full allocation charge, not payload capacity. Reserving 512 B for
// every slot leaves only 4 KiB discretionary space and rejects ordinary
// partially consumed TLS buffers in a 24-connection workload. Keep 8 KiB
// reserved for present/future owners and 12 KiB available above their bases.
constexpr unsigned PhysicalMinimum = 256;
// Atomic RX reserve continues to protect the shared native heap, including
// unbound control/TX allocations. Owned RX gets a separate complete-charge cap.
TcpRxCreditPool pool(12288, BL616CL_TCP_RX_MEMORY_OWNER ? 20480 : 24576, Minimum, Cap, Quantum,
    BL616CL_TCP_RX_PHYSICAL_FAIR ? PhysicalMinimum : 0, 4096, 16);
struct PoolGuard {
#if BL616CL_TCP_RX_MEMORY_OWNER
    uintptr_t flags;
    PoolGuard() : flags(mm_lock_save()) {}
    ~PoolGuard() { mm_unlock_restore(flags); }
#else
    PoolGuard() {}
    ~PoolGuard() {}
#endif
};
#if BL616CL_TCP_RX_MEMORY_OWNER
unsigned admitted, denied, physical_peak, block_peak, type_rejected;
#endif

TcpRxCreditPool::Token token(const tcp_pcb *pcb) {
    return pool.token_for(reinterpret_cast<uintptr_t>(pcb));
}
unsigned window(const tcp_pcb *pcb) {
    LWIP_ASSERT_CORE_LOCKED();
    const PoolGuard guard;
    TcpRxCreditPool::View state;
    return pool.view(token(pcb), &state) ? state.outstanding + state.unread : 0;
}
int opened(tcp_pcb *pcb) {
    LWIP_ASSERT_CORE_LOCKED();
    const PoolGuard guard;
    auto grant = pool.open(reinterpret_cast<uintptr_t>(pcb));
    return grant.token && pool.commit(grant);
}
void closed(tcp_pcb *pcb) {
    LWIP_ASSERT_CORE_LOCKED();
    const PoolGuard guard;
    const auto owner = token(pcb);
    if (owner) pool.retire(owner); // failed admission has no owner to retire
}
void received(tcp_pcb *pcb, unsigned bytes) {
    LWIP_ASSERT_CORE_LOCKED();
    const PoolGuard guard;
    const auto owner = token(pcb);
    if (!pool.received(owner, bytes)) return;
    // A short application/control message does not borrow the entire idle
    // window. A flow consuming its minimum can ask for discretionary credit.
    if (bytes && pcb->rcv_wnd <= Minimum / 2 && pcb->state == ESTABLISHED)
        pool.request(owner, Cap);
}
void pump() {
    LWIP_ASSERT_CORE_LOCKED();
    // Native TIME_WAIT input only ACKs/discards segments; it cannot deliver new
    // sequence bytes. Release its sequence promise after native registration,
    // without changing PCB lifetime, rcv_wnd or advertised wire fields. Actual
    // allocations keep their retired owner until the last native free.
    for (tcp_pcb *pcb = tcp_tw_pcbs; pcb; pcb = pcb->next) {
        const PoolGuard guard;
        const auto owner = token(pcb);
        if (owner) pool.retire(owner);
    }
    tcp_pcb *changed[TcpRxCreditPool::FlowCount] = {};
    unsigned count = 0;
    // Every successful iteration grants >0. Bound work even after misuse.
    for (unsigned n = 0; n < TcpRxCreditPool::FlowCount * (Cap / Quantum + 1); ++n) {
        const PoolGuard guard;
        auto grant = pool.next_grant();
        if (!grant.token) break;
        TcpRxCreditPool::View state;
        if (!pool.view(grant.token, &state)) { pool.cancel(grant); break; }
        auto *pcb = reinterpret_cast<tcp_pcb *>(state.pcb);
        if (pcb->state != ESTABLISHED) {
            pool.cancel(grant);
            pool.request(grant.token, 0);
            continue;
        }
        if (!pool.commit(grant)) break; // reserve/commit before native expansion
        pcb->rcv_wnd += grant.bytes;
        bool found = false;
        for (unsigned i = 0; i < count; ++i) if (changed[i] == pcb) found = true;
        if (!found && count < TcpRxCreditPool::FlowCount) changed[count++] = pcb;
    }
    for (unsigned i = 0; i < count; ++i) {
        tcp_update_rcv_ann_wnd(changed[i]);
        tcp_ack_now(changed[i]);
        tcp_output(changed[i]); // native input defers output for tcp_input_pcb
    }
}
void recved(tcp_pcb *pcb, unsigned bytes) {
    LWIP_ASSERT_CORE_LOCKED();
    // Buffered netconn data may be consumed after native TIME_WAIT entry.
    // Its physical charge remains tracked independently; no sequence credit
    // can be advertised for this terminal connection.
    if (pcb->state == TIME_WAIT) { pump(); return; }
    bool returned;
    {
        const PoolGuard guard;
        returned = pool.recved(token(pcb), bytes);
    }
    if (returned) pump(); // no heap/output/core acquisition inside PoolGuard
}
}
namespace bridge {
bool tcp_rx_native_policy_healthy() {
    LWIP_ASSERT_CORE_LOCKED();
    const PoolGuard guard;
    return pool.healthy();
}
}
extern "C" const bridge_tcp_rx_policy_hooks bridge_tcp_rx_policy = {
    opened, closed, window, received, recved, pump
};
#if BL616CL_TCP_RX_MEMORY_OWNER
extern "C" int bridge_tcp_rx_memory_admit(tcp_pcb *pcb, pbuf *packet) {
    LWIP_ASSERT_CORE_LOCKED();
    TcpRxCreditPool::Token owner;
    {
        const PoolGuard guard;
        owner = token(pcb);
    }
    for (pbuf *part = packet; part; part = part->next) {
        // This input owns a unique native PBUF_RAM reference. A custom/pool
        // allocation must not be interpreted as a native heap allocation.
        // pbuf_init_alloced_pbuf stores only the low byte of pbuf_type;
        // allocation-only PBUF_RAM flags (e.g. 0x0200) are not retained there.
        if (part->type_internal != static_cast<uint8_t>(PBUF_RAM) || part->ref != 1 ||
            (part->flags & PBUF_FLAG_IS_CUSTOM)) {
            const PoolGuard guard;
            ++type_rejected; ++denied;
            return 0;
        }
        // Query takes the native heap lock with IRQs enabled. This pbuf's local
        // reference keeps its allocation alive until admission or rejection.
        const unsigned charge = bridge_lwip_mem_charge(part);
        const PoolGuard guard;
        if (!charge || !pool.retain(owner, reinterpret_cast<uintptr_t>(part), charge)) {
            ++denied;
            return 0; // native caller frees the whole unacknowledged chain
        }
        const auto state = pool.snapshot();
        if (state.physical_used > physical_peak) physical_peak = state.physical_used;
        if (state.blocks > block_peak) block_peak = state.blocks;
    }
    const PoolGuard guard;
    ++admitted;
    return 1;
}
extern "C" void bridge_lwip_mem_lifecycle(void *allocation, unsigned before, unsigned after) {
    // Called under the heap lock, possibly from a netconn/application task.
    // Only bounded metadata operations occur here; never enter the TCP core.
    const PoolGuard guard;
    pool.lifecycle(reinterpret_cast<uintptr_t>(allocation), before, after);
}
#endif
#endif

namespace bridge {
void tcp_rx_physical_report(uint8_t *report) {
    report[5] = 60; report[6] = BL616CL_TCP_RX_PHYSICAL_FAIR; report[7] = 1;
#if BL616CL_TCP_RX_PHYSICAL_FAIR
    const PoolGuard guard;
    const auto state = pool.snapshot();
    const uint32_t values[] = {state.physical_used, pool.physical_reserved(),
        state.blocks, pool.blocks_reserved(), pool.max_owner_charge(),
        pool.max_owner_blocks(), PhysicalMinimum, 4096, 16, 20480, 128, state.faults};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        for (unsigned j = 0; j < 4; ++j) report[8 + 4*i + j] = values[i] >> (8*j);
#endif
}
void tcp_rx_memory_report(uint8_t *report) {
    report[5] = 59; report[6] = BL616CL_TCP_RX_MEMORY_OWNER; report[7] = 1;
#if BL616CL_TCP_RX_MEMORY_OWNER
    const PoolGuard guard;
    const auto state = pool.snapshot();
    const uint32_t values[] = {state.sequence_used, state.physical_used,
        state.future_reserved, state.minimum_reserved, state.live_flows,
        state.retired_flows, state.blocks, state.faults, admitted, denied,
        physical_peak, block_peak, type_rejected};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        for (unsigned j = 0; j < 4; ++j) report[8 + 4*i + j] = values[i] >> (8*j);
#endif
}
}
