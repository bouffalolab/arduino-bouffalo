#!/usr/bin/env python3
"""Exercise actual optional PTI writes against a mapped register page."""
from pathlib import Path
import subprocess
import tempfile
from bl616cl_sta_disconnect_wait_test import function

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'UNOR4BL616CLBridge/coex_diagnostic.cpp').read_text()
state = source[source.index('#if defined(BL616CL_AP_SCAN_PTI)'):source.index('CoexDiagnostic coex_diagnostic_snapshot()')]
functions = ''
prefix = '''#include <cstdint>
#include <cassert>
#include <sys/mman.h>
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define BLE_EVENT_ACTIVE_SCANNING 4
#define BLE_EVENT_PASSIVE_SCANNING 7
static bool ap_enabled=false;
static uint32_t priorities[8]={0,0,0,0,9,0,0,3};
void coex_apply_ap_scan_priority(bool ap_start_confirmed=false);
int wifi_mgmr_ap_state_get() { return ap_enabled; }
uint32_t btble_controller_get_event_priority(uint8_t event) { return priorities[event]; }
int btble_controller_set_event_priority(uint8_t event,uint8_t value) { priorities[event]=value;return 0; }
'''
main = '''
int main() {
 void *page=mmap((void *)0x24b00000U,4096,PROT_READ|PROT_WRITE,
                MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
 assert(page!=(void *)-1);
 auto &pti=*(volatile uint32_t *)0x24b00404U;
 auto &neighbor=*(volatile uint32_t *)0x24b00400U;
 neighbor=0xabcdef01U;pti=0x3024f637U;
 coex_restore_ap_beacon_priority();assert(pti==0x3024f637U);
 coex_apply_ap_beacon_priority();
#ifdef BL616CL_AP_BEACON_PTI
 assert(pti==((uint32_t(BL616CL_AP_BEACON_PTI)<<28)|0x0024f637U));
#else
 assert(pti==0x3024f637U);
#endif
 coex_apply_ap_beacon_priority();
 // Another owner changes a different priority while the override is active.
 pti=(pti&0xfffffff0U)|5U;
 coex_restore_ap_beacon_priority();
 assert(pti==0x3024f635U && neighbor==0xabcdef01U);
 coex_restore_ap_beacon_priority();assert(pti==0x3024f635U);
 coex_apply_ap_scan_priority();assert(priorities[4]==9 && priorities[7]==3);
 // Native success may precede the manager-visible AP state publication.
 coex_apply_ap_scan_priority(true);
#ifdef BL616CL_AP_SCAN_PTI
 assert(priorities[4]==BL616CL_AP_SCAN_PTI && priorities[7]==BL616CL_AP_SCAN_PTI);
#endif
 coex_restore_ap_scan_priority();assert(priorities[4]==9 && priorities[7]==3);
 ap_enabled=true;coex_apply_ap_scan_priority();coex_apply_ap_scan_priority();
#ifdef BL616CL_AP_SCAN_PTI
 assert(priorities[4]==BL616CL_AP_SCAN_PTI && priorities[7]==BL616CL_AP_SCAN_PTI);
#else
 assert(priorities[4]==9 && priorities[7]==3);
#endif
 coex_restore_ap_scan_priority();coex_restore_ap_scan_priority();
 assert(priorities[4]==9 && priorities[7]==3);
 coex_apply_ap_join_scan_priority();
#ifdef BL616CL_AP_JOIN_SCAN_PTI
 assert(priorities[4]==BL616CL_AP_JOIN_SCAN_PTI && priorities[7]==BL616CL_AP_JOIN_SCAN_PTI);
 coex_apply_ap_scan_priority(true);
 assert(priorities[4]==BL616CL_AP_SCAN_PTI && priorities[7]==BL616CL_AP_SCAN_PTI);
 coex_restore_ap_scan_priority();assert(priorities[4]==9 && priorities[7]==3);
#endif
 munmap(page,4096);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory);unit=path/'priority.cpp';unit.write_text(prefix+state+functions+main)
    for flags in ([], ['-DBL616CL_AP_BEACON_PTI=0'], ['-DBL616CL_AP_BEACON_PTI=14'], ['-DBL616CL_AP_BEACON_PTI=14', '-DBL616CL_AP_SCAN_PTI=3'], ['-DBL616CL_AP_BEACON_PTI=14', '-DBL616CL_AP_SCAN_PTI=1'], ['-DBL616CL_AP_BEACON_PTI=14', '-DBL616CL_AP_SCAN_PTI=0'], ['-DBL616CL_AP_BEACON_PTI=14', '-DBL616CL_AP_SCAN_PTI=1', '-DBL616CL_AP_JOIN_SCAN_PTI=0']):
        subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror',*flags,str(unit),'-o',str(path/'priority')],check=True)
        subprocess.run([str(path/'priority')],check=True)
backend = (ROOT / 'UNOR4BL616CLBridge/bouffalo_wifi_backend.cpp').read_text()
ap_task = function(backend, 'void softap_start_task(')
assert 'if (native_result == 0)' in ap_task
success = ap_task[ap_task.index('if (native_result == 0)'):]
assert 'BouffaloHciController::apply_ap_scan_priority_if_active();' in success
assert ap_task.count('BouffaloHciController::apply_ap_scan_priority_if_active();') == 1
assert 'NetworkScanWindow scan_window(hci.pause_scan, hci.resume_scan, hci.context);' in ap_task
assert ap_task.index('NetworkScanWindow scan_window') < ap_task.index('wifi_mgmr_ap_start(&parameters)')
assert ap_task.index('Restore scanning before deleting') < ap_task.index('vTaskDelete(nullptr)')
controller_source = (ROOT / 'UNOR4BL616CLBridge/bouffalo_hci_controller.cpp').read_text()
guard = function(controller_source, 'void BouffaloHciController::apply_ap_scan_priority_if_active(')
factory = function(controller_source, 'BleHciController BouffaloHciController::active_controller(')
join_guard = function(controller_source, 'bool BouffaloHciController::apply_ap_join_scan_priority_if_active(')
unit_text = """#include <atomic>
#include <cassert>
struct BleHciController { void *context = nullptr; };
static unsigned calls=0, join_calls=0;
void coex_apply_ap_join_scan_priority() { ++join_calls; }
void coex_apply_ap_scan_priority(bool confirmed) { assert(confirmed); ++calls; }
class BouffaloHciController {
public:
 static BouffaloHciController *active_instance_;
 bool ready=false;
 std::atomic<bool> scan_active_{false};
 bool initialized() const { return ready; }
 static void apply_ap_scan_priority_if_active();
 static bool apply_ap_join_scan_priority_if_active();
 static BleHciController active_controller();
 BleHciController controller() { BleHciController result;result.context=this;return result; }
};
BouffaloHciController *BouffaloHciController::active_instance_=nullptr;
""" + guard + factory + join_guard + """
int main() {
 assert(BouffaloHciController::active_controller().context==nullptr);
 assert(!BouffaloHciController::apply_ap_join_scan_priority_if_active());
 BouffaloHciController::apply_ap_scan_priority_if_active();assert(calls==0);
 BouffaloHciController c;BouffaloHciController::active_instance_=&c;
 c.scan_active_=true;BouffaloHciController::apply_ap_scan_priority_if_active();assert(calls==0);
 assert(!BouffaloHciController::apply_ap_join_scan_priority_if_active());
 assert(BouffaloHciController::active_controller().context==nullptr);
 c.ready=true;assert(BouffaloHciController::active_controller().context==&c);
 c.scan_active_=false;BouffaloHciController::apply_ap_scan_priority_if_active();assert(calls==0);
 assert(!BouffaloHciController::apply_ap_join_scan_priority_if_active());
 c.scan_active_=true;BouffaloHciController::apply_ap_scan_priority_if_active();
#ifdef BL616CL_AP_SCAN_PTI
 assert(calls==1);
#else
 assert(calls==0);
#endif
#ifdef BL616CL_AP_JOIN_SCAN_PTI
 assert(BouffaloHciController::apply_ap_join_scan_priority_if_active() && join_calls==1);
#else
 assert(!BouffaloHciController::apply_ap_join_scan_priority_if_active() && join_calls==0);
#endif
 c.ready=false;assert(!BouffaloHciController::apply_ap_join_scan_priority_if_active());
}
"""
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory);unit=path/'guard.cpp';unit.write_text(unit_text)
    for flags in ([], ['-DBL616CL_AP_SCAN_PTI=1'], ['-DBL616CL_AP_SCAN_PTI=1', '-DBL616CL_AP_JOIN_SCAN_PTI=0']):
        subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror',*flags,str(unit),'-o',str(path/'guard')],check=True)
        subprocess.run([str(path/'guard')],check=True)
hold = function(backend, 'void hold_ap_start_scan_window(')
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory);unit=path/'hold.cpp'
    unit.write_text("""#include <cassert>
static unsigned waited=0;
#define pdMS_TO_TICKS(ms) (ms)
void vTaskDelay(unsigned ticks) { waited+=ticks; }
""" + hold + """
int main() {
 hold_ap_start_scan_window(false);assert(waited==0);
 hold_ap_start_scan_window(true);
#ifdef BL616CL_AP_START_SCAN_WINDOW_MS
 assert(waited==BL616CL_AP_START_SCAN_WINDOW_MS);
#else
 assert(waited==0);
#endif
}
""")
    for flags in ([], ['-DBL616CL_AP_START_SCAN_WINDOW_MS=0'], ['-DBL616CL_AP_START_SCAN_WINDOW_MS=5000']):
        subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror',*flags,str(unit),'-o',str(path/'hold')],check=True)
        subprocess.run([str(path/'hold')],check=True)
    for value in ('-1','10001'):
        result=subprocess.run(['c++','-std=c++11','-DBL616CL_AP_START_SCAN_WINDOW_MS='+value,str(unit),'-o',str(path/'hold')],capture_output=True)
        assert result.returncode != 0
print('BL616CL_COEX_BEACON_PRIORITY_PASS')
