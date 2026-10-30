#pragma once
#include <stddef.h>
#include <stdint.h>

namespace bridge {
// Externally serialized policy state. No native PCB/window/pbuf mutation here.
// Cross-task allocator integration must protect every operation with the same
// short critical section; never allocate or acquire core/heap locks within it.
// Integration must commit a grant BEFORE increasing any accepted/announced
// right edge. Retire at native tcp_free or confirmed native TIME_WAIT entry,
// never at application socket close. TIME_WAIT cannot accept sequence data.
// Physical charges are a separate ledger: recved never returns memory credit.
class TcpRxCreditPool {
public:
    typedef uint64_t Token;
    static const size_t FlowCount = 32;
    static const size_t BlockCount = 128;
    struct Grant { Token token; uint32_t bytes; uint64_t ticket; };
    struct View { uintptr_t pcb; uint32_t outstanding, unread; };
    Token token_for(uintptr_t pcb) const;
    bool view(Token token, View *result) const;
    struct Snapshot {
        uint32_t sequence_used, physical_used, future_reserved, minimum_reserved;
        uint32_t live_flows, retired_flows, blocks, faults;
    };

    TcpRxCreditPool(uint32_t sequence_limit, uint32_t physical_limit,
                    uint32_t minimum, uint32_t cap, uint32_t quantum,
                    uint32_t physical_minimum = 0, uint32_t physical_cap = 0,
                    uint32_t block_cap = 0);
    // Returns an uncommitted minimum grant. A failed native open must retire it.
    Grant open(uintptr_t pcb);
    // A zero target suspends future grants without retracting accepted credit.
    bool request(Token token, uint32_t target);
    Grant next_grant();
    bool commit(Grant grant);
    bool cancel(Grant grant);
    bool received(Token token, uint32_t sequence_bytes);
    bool recved(Token token, uint32_t sequence_bytes);
    bool retire(Token token);

    // Native allocation ownership only. charge includes allocator headers and
    // unsplittable slack. Bind before acknowledging data; failure requires the
    // caller to reject/free that unacknowledged ingress allocation. Trimming
    // must report actual remaining charge; final free uses zero. Ref drops and
    // partial payload consumption do not call resize unless charge changes.
    bool retain(Token token, uintptr_t allocation, uint32_t charge);
    bool resize(uintptr_t allocation, uint32_t actual_charge);
    uint32_t allocation_charge(uintptr_t allocation) const;
    uint32_t physical_reserved() const;
    uint32_t blocks_reserved() const;
    uint32_t max_owner_charge() const;
    uint32_t max_owner_blocks() const;
    // Ignore unbound control/TX allocations. A tracked lifecycle mismatch stops
    // grants, while a real final-free still clears the block and tombstone.
    bool lifecycle(uintptr_t allocation, uint32_t before, uint32_t after);
    Snapshot snapshot() const;
    bool healthy() const { return faults_ == 0; }

private:
    struct Flow {
        Token token;
        uint64_t ticket;
        uintptr_t pcb;
        uint32_t outstanding, unread, pending, target, blocks, physical;
        bool live;
    };
    struct Block { Token token; uintptr_t allocation; uint32_t charge; };
    Flow *find(Token token);
    bool fault();
    uint32_t available() const;
    Flow flows_[FlowCount];
    Block blocks_[BlockCount];
    Token generation_;
    uint64_t ticket_;
    uint32_t sequence_limit_, physical_limit_, minimum_, cap_, quantum_;
    uint32_t used_, physical_, live_, faults_;
    uint32_t physical_minimum_, physical_cap_, block_cap_;
    size_t cursor_;
};
}
