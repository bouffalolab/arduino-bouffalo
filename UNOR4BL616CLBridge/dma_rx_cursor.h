#pragma once
#include <stdint.h>

namespace bridge {
// Two equal cyclic descriptors; hardware LLI count is modulo 1024.
// Call at least once per 1024 descriptors. AT requests are serialized and
// send no payload while a network command is pending.
inline bool dma_rx_overwritten(uint32_t previous_count, uint32_t count,
                               uint32_t previous_offset, uint32_t offset)
{
    const uint32_t delta = (count - previous_count) & 1023U;
    return delta > 2U || (delta == 2U && offset >= previous_offset);
}
}
