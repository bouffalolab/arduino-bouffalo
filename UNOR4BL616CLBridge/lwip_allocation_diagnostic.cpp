#include "lwip_allocation_diagnostic.h"
#if BL616CL_LWIP_ALLOC_FAILURE_DIAGNOSTIC
extern "C" {
#include "sdk/mm/mm.h"
}
namespace {
uint32_t count;
// Sequence, request, aligned request, free payload, largest hole, hole count,
// walk valid, charged usage, native error count, caller return PC.
uint32_t entries[4][10];
}
extern "C" void bridge_lwip_mem_failure(unsigned requested, unsigned aligned,
    unsigned free_bytes, unsigned largest, unsigned holes, unsigned valid,
    unsigned used, unsigned errors, unsigned long caller) {
    const uintptr_t flags = mm_lock_save();
    uint32_t *entry = entries[count % 4];
    entry[0] = ++count;
    entry[1] = requested; entry[2] = aligned; entry[3] = free_bytes;
    entry[4] = largest; entry[5] = holes; entry[6] = valid;
    entry[7] = used; entry[8] = errors; entry[9] = caller;
    mm_unlock_restore(flags);
}
#endif
namespace bridge {
void lwip_allocation_diagnostic_report(uint8_t page, uint8_t *report) {
    report[5] = page;
    report[6] = BL616CL_LWIP_ALLOC_FAILURE_DIAGNOSTIC ? 1 : 0;
    report[7] = 1;
#if BL616CL_LWIP_ALLOC_FAILURE_DIAGNOSTIC
    uint32_t snapshot[10] = {};
    const uintptr_t flags = mm_lock_save();
    if (page == 53) {
        snapshot[0] = count;
        snapshot[1] = 4;
    } else if (page >= 54 && page <= 57) {
        for (unsigned i = 0; i < 10; ++i) snapshot[i] = entries[page - 54][i];
    }
    mm_unlock_restore(flags);
    for (unsigned i = 0; i < 10; ++i)
        for (unsigned b = 0; b < 4; ++b) report[8 + 4*i + b] = snapshot[i] >> (8*b);
#endif
}
}
