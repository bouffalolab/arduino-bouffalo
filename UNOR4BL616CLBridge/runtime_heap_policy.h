#pragma once
#include <stddef.h>
#include <stdint.h>

namespace bridge {
// Soft reserves for ordinary ANY allocations, not guarantees for explicit
// allocations, realloc growth, fragmentation, or supplier/ROM internals.
inline uint32_t preferred_runtime_heap(size_t size, size_t ocram_free, size_t wram_free)
{
    constexpr size_t overhead_margin = 512;
    const bool ocram_room = ocram_free >= 8192 + overhead_margin &&
        size <= ocram_free - 8192 - overhead_margin;
    const bool wram_room = wram_free >= 12288 + overhead_margin &&
        size <= wram_free - 12288 - overhead_margin;
    if (size <= 512 && ocram_room) return 2;
    if (wram_room) return 3;
    if (ocram_room) return 2;
    return 0; // Preserve original fallback rather than manufacture an OOM.
}

#if defined(BL616CL_RUNTIME_BALANCED_HEAP) && BL616CL_RUNTIME_BALANCED_HEAP
void runtime_heap_policy_enable(bool enabled);
#else
inline void runtime_heap_policy_enable(bool) {}
#endif
}
