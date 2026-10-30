#include "runtime_heap_policy.h"

#if defined(BL616CL_RUNTIME_BALANCED_HEAP) && BL616CL_RUNTIME_BALANCED_HEAP
extern "C" {
#include "sdk/mm/mm.h"
}
static_assert(MM_HEAP_OCRAM_0 == 2 && MM_HEAP_WRAM_0 == 3, "heap policy ABI");
namespace {
volatile bool policy_active = false;
}
namespace bridge {
void runtime_heap_policy_enable(bool enabled) { policy_active = enabled; }
}
extern "C" int fhost_bridge_heap_policy_active() { return policy_active; }
extern "C" uint32_t fhost_bridge_heap_preference(
    size_t size, uint32_t, size_t ocram_free, size_t wram_free)
{
    return bridge::preferred_runtime_heap(size, ocram_free, wram_free);
}
#endif
