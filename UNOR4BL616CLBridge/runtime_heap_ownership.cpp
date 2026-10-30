#include "runtime_heap_ownership.h"
#include <string.h>
#include "sdk/mm/mm.h"
#include "FreeRTOS.h"
#include "task.h"

namespace bridge {
namespace {
#if defined(BL616CL_RUNTIME_HEAP_OWNERSHIP) && BL616CL_RUNTIME_HEAP_OWNERSHIP
void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8U * i));
}
struct Row {
    uint32_t address;
    uint32_t size;
    uint8_t kind; // 0 unknown, 1 task stack, 2 task control block
    char name[15];
};
static_assert(sizeof(Row) == 24, "ownership report ABI");
struct Snapshot {
    Row rows[8];
    uint32_t sequence;
    uint16_t blocks;
    uint8_t tasks;
    uint8_t valid;
};
Snapshot snapshot = {};
volatile bool pending = false;
struct Context {
    Snapshot *sample;
    TaskStatus_t *tasks;
    UBaseType_t count;
};
void walk(mm_heap_t *, void *ptr, size_t size, void *user) {
    auto &context = *static_cast<Context *>(user);
    auto &sample = *context.sample;
    if (sample.blocks != UINT16_MAX) ++sample.blocks;
    Row row = {};
    row.address = reinterpret_cast<uintptr_t>(ptr);
    row.size = size;
    for (UBaseType_t i = 0; i < context.count; ++i) {
        const auto &task = context.tasks[i];
        if (ptr == task.pxStackBase || ptr == task.xHandle) {
            row.kind = ptr == task.pxStackBase ? 1U : 2U;
            // Names are copied while the scheduler is suspended.
            if (task.pcTaskName) strncpy(row.name, task.pcTaskName, sizeof(row.name) - 1U);
            break;
        }
    }
    for (unsigned i = 0; i < 8; ++i) {
        if (row.size < sample.rows[i].size ||
            (row.size == sample.rows[i].size && row.address >= sample.rows[i].address)) continue;
        for (unsigned j = 7; j > i; --j) sample.rows[j] = sample.rows[j - 1];
        sample.rows[i] = row;
        break;
    }
}
#endif
}

void runtime_heap_ownership_request() {
#if defined(BL616CL_RUNTIME_HEAP_OWNERSHIP) && BL616CL_RUNTIME_HEAP_OWNERSHIP
    pending = true;
#endif
}
void runtime_heap_ownership_poll() {
#if defined(BL616CL_RUNTIME_HEAP_OWNERSHIP) && BL616CL_RUNTIME_HEAP_OWNERSHIP
    if (!pending) return;
    pending = false;
    Snapshot sample = {};
    TaskStatus_t tasks[24] = {};
    vTaskSuspendAll();
    const UBaseType_t count = uxTaskGetSystemState(tasks, 24U, nullptr);
    sample.tasks = count;
    const uintptr_t flags = mm_lock_save();
    if (count != 0U && g_mem_manager.initialized) {
        auto *heap = g_mem_manager.heaps[MM_HEAP_OCRAM_0];
        if (heap && heap->is_active && heap->allocator_id < CONFIG_MM_ALLOCATOR_COUNT) {
            const auto *allocator = g_mem_manager.allocators[heap->allocator_id];
            if (allocator && allocator->walk_allocated_blocks) {
                Context context = {&sample, tasks, count};
                allocator->walk_allocated_blocks(heap, walk, &context);
                sample.valid = 1U;
            }
        }
    }
    sample.sequence = snapshot.sequence + 1U;
    snapshot = sample;
    mm_unlock_restore(flags);
    xTaskResumeAll();
#endif
}
void runtime_heap_ownership_report(uint8_t page, uint8_t *report) {
    report[5] = page;
#if defined(BL616CL_RUNTIME_HEAP_OWNERSHIP) && BL616CL_RUNTIME_HEAP_OWNERSHIP
    const uintptr_t flags = mm_lock_save();
    report[6] = snapshot.valid;
    report[7] = 8U;
    put32(&report[8], snapshot.sequence);
    report[12] = snapshot.blocks;
    report[13] = snapshot.blocks >> 8U;
    report[14] = snapshot.tasks;
    memcpy(&report[16], &snapshot.rows[(page - 42U) * 2U], 48U);
    mm_unlock_restore(flags);
#else
    (void)report;
#endif
}
}
