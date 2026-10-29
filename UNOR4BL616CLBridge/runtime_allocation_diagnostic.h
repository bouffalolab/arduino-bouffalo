#ifndef BRIDGE_RUNTIME_ALLOCATION_DIAGNOSTIC_H
#define BRIDGE_RUNTIME_ALLOCATION_DIAGNOSTIC_H
#include <stdint.h>

namespace bridge {
struct RuntimeAllocationRow {
    uint32_t calls;
    uint32_t failures;
    uint32_t max_request;
    uint32_t max_failed_request;
};
// Rows: completed ANY/OCRAM/WRAM/other requests, then OCRAM/WRAM attempts.
// Caller holds the raw IRQ lock. No allocation, heap walks or RTOS calls.
class RuntimeAllocationDiagnostic {
public:
    RuntimeAllocationRow rows[6] = {};
    void event(uint8_t stage, uint32_t a, uint32_t b, uint32_t c) {
        if (stage == 13U) attempt_heap = c;
        if (stage == 11U) {
            const uint32_t heap = c & 15U;
            record(heap == 0U ? 0U : heap == 2U ? 1U : heap == 3U ? 2U : 3U, b, a == 0U);
        } else if (stage == 15U && attempt_heap >= 2U && attempt_heap <= 3U) {
            record(2U + attempt_heap, b, a == 0U);
        }
    }
private:
    uint32_t attempt_heap = 0U;
    void record(uint32_t index, uint32_t size, bool failed) {
        auto &row = rows[index];
        if (row.calls != UINT32_MAX) ++row.calls;
        if (size > row.max_request) row.max_request = size;
        if (failed) {
            if (row.failures != UINT32_MAX) ++row.failures;
            if (size > row.max_failed_request) row.max_failed_request = size;
        }
    }
};
bool runtime_allocation_snapshot(uint8_t page, RuntimeAllocationRow *rows);
}
#endif
