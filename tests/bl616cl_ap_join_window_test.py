#!/usr/bin/env python3
"""Exercise join completion/timeout against the actual worker helper."""
from pathlib import Path
import subprocess
import tempfile
from bl616cl_sta_disconnect_wait_test import function
root = Path(__file__).resolve().parents[1]
source = (root/'UNOR4BL616CLBridge/bouffalo_wifi_backend.cpp').read_text()
helper = function(source, 'void finish_ap_join_scan_window(')
# Adaptive mode must wait while the RAII RF window still owns the pause.
worker = function(source, 'void softap_start_task(')
adaptive = worker.index('#if defined(BL616CL_AP_JOIN_SCAN_PAUSE)', worker.index('hold_ap_start_scan_window'))
restore = worker.index('} // Restore scanning')
assert adaptive < worker.index('finish_ap_join_scan_window(', adaptive) < restore
assert 'join_window = false;' in worker[adaptive:restore]

with tempfile.TemporaryDirectory() as temp:
    path = Path(temp)
    (path/'test.cpp').write_text(r'''
#include <cassert>
#include <cstdint>
#include "coex_diagnostic.h"
using bridge::ap_join_scan_counters;
static uint32_t ticks, ack_at;
static unsigned restores;
uint32_t millis() { return ticks; }
#define pdMS_TO_TICKS(ms) (ms)
void vTaskDelay(unsigned n) {
 ticks+=n;
 if (ack_at && ticks>=ack_at) ++ap_join_scan_counters().client_ip_reply_acknowledged;
}
struct BouffaloHciController {
 static void apply_ap_scan_priority_if_active() { ++restores; }
};
''' + helper + r'''
int main() {
#ifdef BL616CL_AP_JOIN_SCAN_TIMEOUT_MS
 const unsigned expected_timeout=BL616CL_AP_JOIN_SCAN_TIMEOUT_MS;
#else
 const unsigned expected_timeout=5000;
#endif
 auto &c=ap_join_scan_counters();
 c.active=1;finish_ap_join_scan_window(false,0);
 assert(c.active==0 && ticks==0 && restores==0);
 c.active=1;ack_at=100;finish_ap_join_scan_window(true,0);
 assert(c.active==0 && c.completed==1 && c.timeouts==0 && c.elapsed_ms==100 && restores==1);
 c.active=1;ack_at=0;ticks=0;finish_ap_join_scan_window(true,c.client_ip_reply_acknowledged);
 assert(c.active==0 && c.completed==1 && c.timeouts==1 && c.elapsed_ms==expected_timeout && restores==2);
 // A completion arriving before the worker starts waiting needs no delay.
 c.active=1;ticks=0;finish_ap_join_scan_window(true,c.client_ip_reply_acknowledged-1);
 assert(ticks==0 && c.completed==2 && restores==3 && c.active==0);
}
''')
    command=['c++','-std=c++11','-Wall','-Wextra','-Werror','-I'+str(root/'UNOR4BL616CLBridge'),str(path/'test.cpp'),'-o',str(path/'test')]
    for flags in ([], ['-DBL616CL_AP_JOIN_SCAN_TIMEOUT_MS=10000']):
        subprocess.run(command+flags,check=True)
        subprocess.run([str(path/'test')],check=True)
    invalid=subprocess.run(command+['-DBL616CL_AP_JOIN_SCAN_TIMEOUT_MS=10001'],capture_output=True)
    assert invalid.returncode != 0 and b'bounded to ten seconds' in invalid.stderr
print('BL616CL_AP_JOIN_WINDOW_PASS')
