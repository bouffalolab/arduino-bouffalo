#include "tcp_rx_credit_pool.h"
#include <limits.h>

namespace bridge {
TcpRxCreditPool::TcpRxCreditPool(uint32_t sequence_limit, uint32_t physical_limit,
    uint32_t minimum, uint32_t cap, uint32_t quantum,
    uint32_t physical_minimum, uint32_t physical_cap, uint32_t block_cap)
    : flows_{}, blocks_{}, generation_(0), ticket_(0), sequence_limit_(sequence_limit),
      physical_limit_(physical_limit), minimum_(minimum), cap_(cap), quantum_(quantum),
      used_(0), physical_(0), live_(0), faults_(0),
      physical_minimum_(physical_minimum), physical_cap_(physical_cap),
      block_cap_(block_cap), cursor_(0) {
    if (!minimum || minimum > sequence_limit / FlowCount || minimum > cap ||
        !quantum || !physical_limit ||
        (physical_minimum && (physical_minimum > physical_limit / FlowCount ||
            physical_cap < physical_minimum || physical_cap > physical_limit ||
            !block_cap || block_cap > BlockCount))) fault();
}

TcpRxCreditPool::Token TcpRxCreditPool::token_for(uintptr_t pcb) const {
    if (pcb) for (const auto &flow : flows_)
        if (flow.live && flow.pcb == pcb) return flow.token;
    return 0;
}

bool TcpRxCreditPool::view(Token token, View *result) const {
    if (token && result) for (const auto &flow : flows_)
        if (flow.live && flow.token == token) {
            *result = {flow.pcb, flow.outstanding, flow.unread};
            return true;
        }
    return false;
}

bool TcpRxCreditPool::fault() {
    if (faults_ != UINT32_MAX) ++faults_;
    return false;
}

TcpRxCreditPool::Flow *TcpRxCreditPool::find(Token token) {
    if (!token) return nullptr;
    for (auto &flow : flows_) if (flow.token == token) return &flow;
    return nullptr;
}

uint32_t TcpRxCreditPool::available() const {
    uint32_t reserved = minimum_ * (FlowCount - live_);
    for (const auto &flow : flows_) {
        const uint32_t held = flow.outstanding + flow.unread + flow.pending;
        if (flow.live && held < minimum_) reserved += minimum_ - held;
    }
    return used_ <= sequence_limit_ && reserved <= sequence_limit_ - used_
        ? sequence_limit_ - used_ - reserved : 0;
}

TcpRxCreditPool::Grant TcpRxCreditPool::open(uintptr_t pcb) {
    if (!healthy() || !pcb) return {0, 0, 0};
    for (const auto &flow : flows_)
        if (flow.live && flow.pcb == pcb) { fault(); return {0, 0, 0}; }
    // Never wrap the generation: a delayed release must not match a new flow.
    if (generation_ == UINT64_MAX || ticket_ == UINT64_MAX) { fault(); return {0, 0, 0}; }
    for (auto &flow : flows_) {
        if (flow.token) continue; // retired flows may still own native blocks
        if (minimum_ > sequence_limit_ - used_) { fault(); return {0, 0, 0}; }
        flow = {++generation_, ++ticket_, pcb, 0, 0, minimum_, minimum_, 0, 0, true};
        ++live_;
        used_ += minimum_;
        return {flow.token, minimum_, flow.ticket};
    }
    // Bounded metadata exhaustion rejects admission, not existing cleanup.
    return {0, 0, 0};
}

bool TcpRxCreditPool::request(Token token, uint32_t target) {
    Flow *flow = find(token);
    if (!flow || !flow->live || (target && target < minimum_) || target > cap_) return fault();
    // Lowering a target never retracts a previously committed grant.
    flow->target = target;
    return true;
}

TcpRxCreditPool::Grant TcpRxCreditPool::next_grant() {
    if (!healthy()) return {0, 0, 0};
    const uint32_t free = available();
    for (size_t n = 0; n < FlowCount; ++n) {
        const size_t i = (cursor_ + n) % FlowCount;
        Flow &flow = flows_[i];
        const uint32_t occupied = flow.outstanding + flow.unread;
        if (!flow.live || flow.pending || flow.target <= occupied) continue;
        const uint32_t own_minimum = occupied < minimum_ ? minimum_ - occupied : 0;
        const uint32_t capacity = free + own_minimum;
        if (!capacity) continue;
        if (ticket_ == UINT64_MAX) { fault(); return {0, 0, 0}; }
        uint32_t bytes = flow.target - occupied;
        if (bytes > quantum_) bytes = quantum_;
        if (bytes > capacity) bytes = capacity;
        flow.pending = bytes;
        flow.ticket = ++ticket_;
        used_ += bytes; // reserve before the native caller can advertise
        cursor_ = (i + 1) % FlowCount;
        return {flow.token, bytes, flow.ticket};
    }
    return {0, 0, 0};
}

bool TcpRxCreditPool::commit(Grant grant) {
    Flow *flow = find(grant.token);
    if (!flow || !flow->live || !grant.bytes || flow->pending != grant.bytes ||
        flow->ticket != grant.ticket)
        return fault();
    flow->outstanding += grant.bytes;
    flow->pending = 0;
    return true;
}

bool TcpRxCreditPool::cancel(Grant grant) {
    Flow *flow = find(grant.token);
    if (!flow || !flow->live || !grant.bytes || flow->pending != grant.bytes ||
        flow->ticket != grant.ticket)
        return fault();
    used_ -= flow->pending;
    flow->pending = 0;
    return true;
}

bool TcpRxCreditPool::received(Token token, uint32_t bytes) {
    Flow *flow = find(token);
    if (!flow || !flow->live || bytes > flow->outstanding) return fault();
    flow->outstanding -= bytes;
    flow->unread += bytes;
    return true;
}

bool TcpRxCreditPool::recved(Token token, uint32_t bytes) {
    Flow *flow = find(token);
    if (!flow || !flow->live || bytes > flow->unread) return fault();
    flow->unread -= bytes;
    used_ -= bytes;
    return true;
}

bool TcpRxCreditPool::retire(Token token) {
    Flow *flow = find(token);
    if (!flow || !flow->live) return fault();
    used_ -= flow->outstanding + flow->unread + flow->pending;
    flow->outstanding = flow->unread = flow->pending = flow->target = 0;
    flow->live = false;
    --live_;
    if (!flow->blocks) *flow = {};
    return true;
}

uint32_t TcpRxCreditPool::physical_reserved() const {
    if (!physical_minimum_) return 0;
    uint32_t reserved = physical_minimum_ * FlowCount;
    // Retired owners still occupy a metadata slot until final free. Keep their
    // charge in this calculation so retirement cannot create unfunded reserve.
    for (const auto &flow : flows_)
        reserved -= flow.physical < physical_minimum_ ? flow.physical : physical_minimum_;
    return reserved;
}

uint32_t TcpRxCreditPool::blocks_reserved() const {
    if (!physical_minimum_) return 0;
    uint32_t reserved = FlowCount;
    for (const auto &flow : flows_)
        if (flow.blocks) --reserved;
    return reserved;
}

uint32_t TcpRxCreditPool::max_owner_charge() const {
    uint32_t value = 0;
    for (const auto &flow : flows_) if (flow.physical > value) value = flow.physical;
    return value;
}
uint32_t TcpRxCreditPool::max_owner_blocks() const {
    uint32_t value = 0;
    for (const auto &flow : flows_) if (flow.blocks > value) value = flow.blocks;
    return value;
}

bool TcpRxCreditPool::retain(Token token, uintptr_t allocation, uint32_t charge) {
    Flow *flow = find(token);
    if (!healthy() || !flow || !flow->live || !allocation || !charge) return false;
    for (const auto &block : blocks_)
        if (block.allocation == allocation) return fault();
    if (charge > physical_limit_ - physical_) return false;
    if (physical_minimum_) {
        // Existing and future owners keep a base share. A non-consuming flow
        // cannot borrow it via tiny allocations, even with sequence space left.
        const uint32_t own = flow->physical < physical_minimum_
            ? physical_minimum_ - flow->physical : 0;
        const uint32_t reserved = physical_reserved() - own;
        if (reserved > physical_limit_ - physical_ ||
            charge > physical_limit_ - physical_ - reserved ||
            charge > physical_cap_ - flow->physical || flow->blocks >= block_cap_)
            return false;
        uint32_t count = 0;
        for (const auto &block : blocks_) if (block.allocation) ++count;
        const uint32_t other_blocks = blocks_reserved() - (flow->blocks ? 0 : 1);
        if (count >= BlockCount || other_blocks >= BlockCount - count) return false;
    }
    for (auto &block : blocks_) {
        if (block.allocation) continue;
        block = {token, allocation, charge};
        ++flow->blocks;
        flow->physical += charge;
        physical_ += charge;
        return true;
    }
    return false;
}

bool TcpRxCreditPool::resize(uintptr_t allocation, uint32_t charge) {
    if (!allocation) return fault();
    for (auto &block : blocks_) {
        if (block.allocation != allocation) continue;
        Flow *flow = find(block.token);
        if (!flow || charge > block.charge) return fault();
        physical_ -= block.charge - charge;
        flow->physical -= block.charge - charge;
        block.charge = charge;
        if (!charge) {
            block = {};
            --flow->blocks;
            if (!flow->live && !flow->blocks) *flow = {};
        }
        return true;
    }
    return fault();
}

uint32_t TcpRxCreditPool::allocation_charge(uintptr_t allocation) const {
    if (allocation) for (const auto &block : blocks_)
        if (block.allocation == allocation) return block.charge;
    return 0;
}

bool TcpRxCreditPool::lifecycle(uintptr_t allocation, uint32_t before, uint32_t after) {
    const uint32_t current = allocation_charge(allocation);
    if (!current) return true; // unbound allocation; no owner receives credit
    const bool valid = before == current && after <= before;
    if (!valid) fault();
    if (after <= current) resize(allocation, after);
    return valid;
}

TcpRxCreditPool::Snapshot TcpRxCreditPool::snapshot() const {
    const uint32_t future = minimum_ * (FlowCount - live_);
    Snapshot result = {used_, physical_, future, future, live_, 0, 0, faults_};
    for (const auto &flow : flows_) {
        const uint32_t held = flow.outstanding + flow.unread + flow.pending;
        if (flow.live && held < minimum_) result.minimum_reserved += minimum_ - held;
    }
    for (const auto &flow : flows_) if (flow.token && !flow.live) ++result.retired_flows;
    for (const auto &block : blocks_) if (block.allocation) ++result.blocks;
    return result;
}
}
