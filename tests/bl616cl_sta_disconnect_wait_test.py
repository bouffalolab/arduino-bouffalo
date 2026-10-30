#!/usr/bin/env python3
"""Exercise the SDK patch's actual teardown wait and callback lifetime."""
from pathlib import Path
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / 'tools/bridge_runtime/profiles/bridge/fhost_patches'
TARGET = 'components/wireless/wifi6/fhost/src/fhost_wpa.c'


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def main():
    with tempfile.TemporaryDirectory() as directory:
        folder = Path(directory)
        target = folder / TARGET
        target.parent.mkdir(parents=True)
        target.write_text((ROOT / 'third_party/bouffalo_sdk' / TARGET).read_text())
        for record in json.loads((PROFILE / 'manifest.json').read_text())['patches']:
            if not any(t['path'] == TARGET for t in record['targets']):
                continue
            for part in (PROFILE / record['path']).read_text().split('--- a/')[1:]:
                if part.splitlines()[0] == TARGET:
                    subprocess.run(['patch', '--batch', '-p1'], cwd=folder,
                                   input='--- a/' + part, text=True, check=True,
                                   stdout=subprocess.DEVNULL)
        source = target.read_text()
        prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define MGMR_VIF_STA 0
#define MGMR_VIF_AP 1
#define FHOST_WPA_EVENT_CB_CNT 2
#define CO_BIT(n) (1U << (n))
#define EV_WIFI 0
#define CODE_WIFI_ON_AP_STARTED 0
#define FHOST_WPA_STATE_CONNECTED 2
#define FHOST_WPA_STATE_NOT_CONNECTED 1
#define FHOST_WPA_STATE_STOPPED 0
enum fhost_wpa_event { FHOST_WPA_CONNECTED, FHOST_WPA_DISCONNECTED,
                      FHOST_WPA_EXIT, FHOST_WPA_INTERFACE_REMOVED };
typedef void (*cb_t)(int, enum fhost_wpa_event, void *, void *);
struct fhost_wpa_target_event { int event; void *task; };
struct fhost_wpa_event_cb { unsigned events; cb_t func; void *arg; };
struct fhost_wpa_vif_tag { int state, network_id; struct fhost_wpa_event_cb cb[2]; };
static struct fhost_wpa_vif_tag vif;
static int depth, notifications, mode, register_failure, last_timeout;
static int command_failure, command_event, wait_calls;
static uint8_t stage;
static uint32_t rtos_protect(void) { ++depth; return depth; }
static void rtos_unprotect(uint32_t p) { assert((int)p == depth); --depth; }
static void *rtos_get_task_handle(void) { return (void *)1; }
static void trace(int first, ...) { (void)first; }
#define fhost_bridge_ap_start_wpa_wait trace
#define fhost_bridge_ap_start_wpa_event_path trace
#define fhost_bridge_ap_start_hostapd_lifecycle trace
static void fhost_bridge_sta_disconnect_stage(uint8_t value, int result) {
    (void)result; stage = value;
}
static void platform_post_event(int a, int b, int c) { (void)a; (void)b; (void)c; }
static struct fhost_wpa_vif_tag *fhost_wpa_get_vif(int index) { (void)index; return &vif; }
static int fhost_wpa_cb_unregister(int index, cb_t cb) {
    (void)index;
    for (int i = 0; i < 2; ++i) if (vif.cb[i].func == cb) {
        vif.cb[i].events = 0; vif.cb[i].func = NULL; vif.cb[i].arg = NULL;
        return 0;
    }
    return -1;
}
static void rtos_task_notify(void *task, bool isr) {
    (void)task; (void)isr;
    // The waiting task cannot resume/unwind inside its callback.
    assert(depth > 0); ++notifications;
}
static void fhost_wpa_call_event_cb(int, enum fhost_wpa_event, void *);
'''
        callback = function(source, 'static void fhost_wpa_wait_event(int')
        dispatcher = function(source, 'static void fhost_wpa_call_event_cb(int')
        mocks = r'''
static int fhost_wpa_wait_event_register(int index, enum fhost_wpa_event event,
                                       struct fhost_wpa_target_event *target) {
    assert(index == MGMR_VIF_STA && depth > 0);
    if (register_failure) return -1;
    target->event = event; target->task = (void *)1;
    vif.cb[0].events = CO_BIT(FHOST_WPA_DISCONNECTED) | CO_BIT(FHOST_WPA_EXIT);
    vif.cb[0].func = fhost_wpa_wait_event; vif.cb[0].arg = target;
    return 0;
}
static int fhost_wpa_wait_event_unregister(int index) {
    assert(depth > 0); return fhost_wpa_cb_unregister(index, fhost_wpa_wait_event);
}
static int fhost_wpa_execute_cmd(int index, char *resp, int *len, const char *fmt, ...) {
    (void)index; (void)resp; (void)len; (void)fmt;
    assert(depth == 0);
    if (command_event) {
        vif.state = FHOST_WPA_STATE_NOT_CONNECTED;
        fhost_wpa_call_event_cb(0, FHOST_WPA_DISCONNECTED, NULL);
    }
    return command_failure;
}
static int rtos_task_wait_notification(int timeout) {
    assert(depth == 0); last_timeout = timeout; ++wait_calls;
    if (command_event) return notifications;
    if (mode == 1) {
        vif.state = FHOST_WPA_STATE_NOT_CONNECTED;
        fhost_wpa_call_event_cb(0, FHOST_WPA_DISCONNECTED, NULL);
        return notifications;
    }
    if (mode == 2) {
        vif.state = FHOST_WPA_STATE_STOPPED;
        fhost_wpa_call_event_cb(0, FHOST_WPA_EXIT, NULL);
        return notifications;
    }
    if (mode == 3) return 1; // Stale unrelated notification.
    return 0;
}
'''
        disable = function(source, 'int fhost_wpa_disable_network(int')
        checks = r'''
static void reset(void) {
    vif = (struct fhost_wpa_vif_tag){.state=FHOST_WPA_STATE_CONNECTED, .network_id=1};
    depth=notifications=mode=register_failure=command_failure=command_event=wait_calls=0;
}
int main(void) {
    reset(); mode=1;
    assert(fhost_wpa_disable_network(0) == 0);
    assert(last_timeout == 10000 && notifications == 1 && depth == 0);
    assert(vif.cb[0].func == NULL);
    reset();
    assert(fhost_wpa_disable_network(0) == -1 && stage == 16);
    assert(last_timeout == 10000 && vif.cb[0].arg == NULL && depth == 0);
    fhost_wpa_call_event_cb(0, FHOST_WPA_DISCONNECTED, NULL);
    assert(notifications == 0); // Late event never sees an expired stack target.
    reset(); mode=1; // Recovery after timeout uses a fresh target.
    assert(fhost_wpa_disable_network(0) == 0 && notifications == 1);
    reset(); command_failure=1;
    assert(fhost_wpa_disable_network(0) == -1 && wait_calls == 0);
    assert(vif.cb[0].arg == NULL && depth == 0);
    reset(); register_failure=1;
    assert(fhost_wpa_disable_network(0) == -1 && wait_calls == 0 && depth == 0);
    reset(); command_event=1; // Event may precede the command reply.
    assert(fhost_wpa_disable_network(0) == 0 && notifications == 1 && depth == 0);
    reset(); mode=3;
    assert(fhost_wpa_disable_network(0) == -1 && stage == 17);
    assert(vif.cb[0].arg == NULL && depth == 0);
    fhost_wpa_call_event_cb(0, FHOST_WPA_DISCONNECTED, NULL);
    assert(notifications == 0);
    reset(); mode=2;
    assert(fhost_wpa_disable_network(0) == -1 && vif.cb[0].arg == NULL && depth == 0);
    return 0;
}
'''
        test = folder / 'wait.c'
        test.write_text(prefix + callback + dispatcher + mocks + disable + checks)
        binary = folder / 'wait'
        subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                        str(test), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        (folder / 'bflb_irq.h').write_text(
            '#include <stdint.h>\nuintptr_t bflb_irq_save(void);\n'
            'void bflb_irq_restore(uintptr_t);\n')
        diag = folder / 'diagnostic.cpp'
        implementation = ROOT / 'UNOR4BL616CLBridge/wifi_sta_disconnect_diagnostic.cpp'
        diag.write_text('''#include <assert.h>
#include <stdint.h>
static int depth;
extern "C" uintptr_t bflb_irq_save() { return depth++; }
extern "C" void bflb_irq_restore(uintptr_t old) { assert(depth == int(old)+1); --depth; }
''' + f'#include "{implementation}"\n' + r'''
int main() {
    assert(bridge::wifi_sta_disconnect_diagnostic_snapshot().events == 0);
    fhost_bridge_sta_disconnect_stage(1, 0);
    for (int i = 2; i <= 40; ++i) fhost_bridge_sta_disconnect_stage(i, -1);
    auto snapshot = bridge::wifi_sta_disconnect_diagnostic_snapshot();
    assert(snapshot.attempts == 1 && snapshot.events == 40 && snapshot.count == 16);
    assert(snapshot.stage == 40 && snapshot.result == -1 && depth == 0);
    for (int i = 0; i < 16; ++i)
        assert(snapshot.stages[(snapshot.next + i) % 16] == 25 + i);
    live = {}; // External reset clears RAM history but retains the boundary.
    assert(bridge::wifi_sta_disconnect_diagnostic_snapshot().events == 40);
    assert(bridge::wifi_sta_disconnect_diagnostic_snapshot().stage == 40);
    persistent.magic = 0; // Corrupt/no prior retained record is rejected.
    assert(bridge::wifi_sta_disconnect_diagnostic_snapshot().events == 0);
    fhost_bridge_sta_disconnect_stage(1, 0);
    assert(bridge::wifi_sta_disconnect_diagnostic_snapshot().attempts == 1);
    return 0;
}
''')
        subprocess.run(['c++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(folder), str(diag), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('BL616CL_STA_DISCONNECT_WAIT_PASS')


if __name__ == '__main__':
    main()
