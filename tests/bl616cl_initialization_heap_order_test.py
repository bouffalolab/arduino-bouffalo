"""Run actual runtime state transitions against simulated SDK failures."""
from pathlib import Path
import subprocess
import tempfile
import re

root = Path(__file__).resolve().parents[1]
source = (root / 'UNOR4BL616CLBridge/bridge_runtime.cpp').read_text()
# Keep the production state machine; replace platform headers and functions.
source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.MULTILINE)
platform = r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include "bridge_runtime.h"
#include "runtime_heap_policy.h"
#define CONFIG_MM_HEAP_COUNT 8
#define MM_HEAP_WRAM_0 3
#define MM_HEAP_OCRAM_0 2
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define EV_WIFI 1
#define CODE_WIFI_ON_INIT_DONE 10
#define CODE_WIFI_ON_MGMR_DONE 11
namespace bridge { constexpr bool kEnableWifiBleCoexistence = true;
void BouffaloHciController::set_initialization_priority(uint8_t) {}
void ap_packet_input_hook_begin() {}
}
int scenario, sets, gets;
uint32_t now;
uint32_t order[8] = {0, 1, 2, 3, 0, 0, 0, 0};
int mm_heap_get_auto_list(uint32_t *p, unsigned n) {
    ++gets; assert(n == 8); if (scenario == 4) return -40;
    memcpy(p, order, sizeof order); return 0;
}
int mm_heap_set_auto_list(const uint32_t *p, unsigned n) {
    ++sets;
    if ((scenario == 5 && sets == 1) || (scenario == 6 && sets == 2)) return -42;
    memset(order, 0, sizeof order); memcpy(order, p, n * 4); return 0;
}
uint32_t millis() { return now; }
void bflb_mtd_init() {}
using EfErrCode = int;
constexpr int EF_NO_ERR = 0;
int easyflash_init() { return 0; }
int rfparam_init(int, void *, int) { return scenario == 1 ? -7 : 0; }
int async_register_event_filter(int, void (*)(async_input_event_t, void *), void *) { return 0; }
void tcpip_init(void *, void *) {}
void wifi_task_create() {}
void wifi_mgmr_coex_enable(bool) {}
int fhost_init() { return 0; }
int wifi_mgmr_task_start() { return scenario == 3 ? -8 : 0; }
'''
main = r'''
int main(int argc, char **argv) {
    assert(argc == 2); scenario = atoi(argv[1]);
    bridge::BouffaloHciController hci;
    bridge::BridgeRuntime runtime(hci);
    bool started = runtime.begin(1, 2);
    if (scenario == 4 || scenario == 5) {
        assert(!started && runtime.failure() == bridge::BridgeRuntimeFailure::HeapOrder);
        assert(order[2] == 2 && order[3] == 3);
        assert(sets == (scenario == 4 ? 0 : 1)); return 0;
    }
    if (scenario == 1) {
        assert(!started && runtime.failure() == bridge::BridgeRuntimeFailure::RfInit);
    } else {
        assert(started && order[0] == 3 && order[1] == 2);
        if (scenario == 2) {
            now = 21000; runtime.poll();
            assert(runtime.failure() == bridge::BridgeRuntimeFailure::WifiInitTimeout);
        } else {
            async_event_t event = {10};
            // Dispatch via the existing registered runtime instance.
            bridge::BridgeRuntime::test_dispatch(&event);
            runtime.poll();
            if (scenario == 3) {
                assert(runtime.failure() == bridge::BridgeRuntimeFailure::WifiManagerStart);
            } else {
                event.code = 11; bridge::BridgeRuntime::test_dispatch(&event); runtime.poll();
                if (scenario == 6) {
                    assert(runtime.failed() && runtime.failure() == bridge::BridgeRuntimeFailure::HeapOrder);
                } else {
                    assert(runtime.ready()); assert(runtime.begin(1, 2));
                }
            }
        }
    }
    assert(gets == 1 && sets == 2);
    if (scenario != 6) assert(order[0] == 0 && order[1] == 1 && order[2] == 2 && order[3] == 3);
}
'''
# Use the public SDK registration seam to exercise private event dispatch.
main = main.replace('bridge::BridgeRuntime::test_dispatch(&event)', 'event_callback(&event, nullptr)')
platform = platform.replace('int async_register_event_filter(int, void (*)(async_input_event_t, void *), void *) { return 0; }',
    'void (*event_callback)(async_input_event_t, void *);\nint async_register_event_filter(int, void (*cb)(async_input_event_t, void *), void *) { event_callback = cb; return 0; }')
with tempfile.TemporaryDirectory() as path:
    directory = Path(path)
    (directory / 'async_event.h').write_text('#pragma once\ntypedef struct { unsigned code; } async_event_t;\ntypedef async_event_t *async_input_event_t;\n')
    (directory / 'test.cpp').write_text(platform + source + main)
    executable = directory / 'test'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-DBL616CL_RUNTIME_INIT_WRAM_FIRST=1', '-I' + path,
                    '-I' + str(root / 'UNOR4BL616CLBridge'), str(directory / 'test.cpp'),
                    '-o', str(executable)], check=True)
    for scenario in range(7):
        subprocess.run([str(executable), str(scenario)], check=True)
print('BL616CL_INITIALIZATION_HEAP_ORDER_TRANSITIONS_PASS')
