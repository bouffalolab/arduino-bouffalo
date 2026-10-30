#ifndef BRIDGE_RUNTIME_HEAP_OWNERSHIP_H
#define BRIDGE_RUNTIME_HEAP_OWNERSHIP_H
#include <stdint.h>
namespace bridge {
void runtime_heap_ownership_request();
void runtime_heap_ownership_poll();
void runtime_heap_ownership_report(uint8_t page, uint8_t *report);
}
#endif
