#include "wifi_sta_disconnect_diagnostic.h"
#include <string.h>
extern "C" {
#include "bflb_irq.h"
}

namespace {
constexpr uint16_t kMagic = 0x5344U;
struct Persistent {
    uint16_t magic;
    uint16_t attempts;
    uint16_t events;
    uint8_t stage;
    int8_t result;
};
static_assert(sizeof(Persistent) == 8U, "HBN retained diagnostic budget");
// HBN is nearly full: only the last boundary and counters survive reset.
__attribute__((section(".hbn_noinit.diagnostic"), used)) Persistent persistent;
bridge::WifiStaDisconnectDiagnostic live = {};
}

extern "C" void fhost_bridge_sta_disconnect_stage(uint8_t stage, int result)
{
    const uintptr_t flags = bflb_irq_save();
    if (persistent.magic != kMagic) {
        memset(&persistent, 0, sizeof(persistent));
        persistent.magic = kMagic;
    }
    if (stage == 1U) ++persistent.attempts;
    ++persistent.events;
    persistent.stage = stage;
    persistent.result = static_cast<int8_t>(result);
    live.stages[live.next] = stage;
    live.results[live.next] = static_cast<int8_t>(result);
    live.next = (live.next + 1U) % 16U;
    if (live.count < 16U) ++live.count;
    __asm__ volatile("" ::: "memory");
    bflb_irq_restore(flags);
}

namespace bridge {
WifiStaDisconnectDiagnostic wifi_sta_disconnect_diagnostic_snapshot()
{
    const uintptr_t flags = bflb_irq_save();
    WifiStaDisconnectDiagnostic result = live;
    if (persistent.magic == kMagic) {
        result.attempts = persistent.attempts;
        result.events = persistent.events;
        result.stage = persistent.stage;
        result.result = persistent.result;
    }
    bflb_irq_restore(flags);
    return result;
}
}
