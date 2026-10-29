#!/usr/bin/env python3
"""Exercise the actual HCI controller with asynchronous SDK completions."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
APP = ROOT / "UNOR4BL616CLBridge"

with tempfile.TemporaryDirectory(prefix="bl616cl-scan-window-") as directory:
    work = pathlib.Path(directory)
    (work / "FreeRTOS.h").write_text(
        "#include <stdint.h>\n#define portTICK_PERIOD_MS 1\n"
        "#define pdMS_TO_TICKS(x) (x)\n")
    (work / "task.h").write_text(
        "uint32_t xTaskGetTickCount(void); void vTaskDelay(uint32_t);\n")
    (work / "btble_lib_api.h").write_text(
        "void btble_controller_init(uint8_t); void btble_controller_deinit(void);"
        "void btble_controller_reset(void);\n")
    (work / "hci_onchip.h").write_text((ROOT / "build/bridge-runtime/tools/sdk/"
        "bl616cl/bridge/include/bluetooth/controller/hci_onchip.h").read_text())
    (work / "test.cpp").write_text(r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <vector>
#define private public
#include "bouffalo_hci_controller.h"
#undef private
#include "coex_diagnostic.h"
extern "C" {
#include "hci_onchip.h"
}
static bt_hci_recv_cb receive;
static uint32_t ticks;
static int host_events;
static bool pending, reject_send, defer_completion;
static uint8_t completion_status;
static std::vector<uint8_t> enables, filters;
extern "C" {
void btble_controller_init(uint8_t) {}
void btble_controller_deinit() {}
void btble_controller_reset() {}
uint8_t bt_onchiphci_interface_init(bt_hci_recv_cb cb) { receive=cb; return 0; }
uint16_t bt_onchiphci_handle_rx_acl(void*, uint8_t*) { return 0; }
int8_t bt_onchiphci_send(uint8_t, uint16_t, hci_pkt_struct* p) {
    if (reject_send) return -1;
    assert(p->p.hci_cmd.opcode == 0x200c);
    enables.push_back(p->p.hci_cmd.params[0]);
    filters.push_back(p->p.hci_cmd.params[1]);
    pending=true;
    return 0;
}
uint32_t xTaskGetTickCount() { return ticks; }
void vTaskDelay(uint32_t n) {
    ticks+=n;
    if (pending && !defer_completion) {
        pending=false;
        receive(BT_HCI_CMD_CMP_EVT, 0x200c, &completion_status, 1);
    }
}
}
static void host_receive(void*, const uint8_t*, size_t) { ++host_events; }
int main() {
    bridge::BouffaloHciController controller;
    auto hci=controller.controller();
    assert(hci.start(hci.context, host_receive, nullptr));
    assert(!hci.pause_scan(hci.context)); // Idle BLE must stay idle.
    assert(enables.empty());
    auto &stats=bridge::scan_window_counters();
    assert(stats.pause_attempts.load() == 0);
    const uint8_t enable[]={1,0x0c,0x20,2,1,1};
    assert(hci.send(hci.context,enable,sizeof(enable)));
    vTaskDelay(1);
    assert(host_events == 1);
    assert(hci.pause_scan(hci.context));
    assert(host_events == 1); // Private disable completion is swallowed.
    assert(!hci.pause_scan(hci.context)); // No nested pause.
    hci.resume_scan(hci.context);
    assert(host_events == 1);
    assert((enables == std::vector<uint8_t>{1,0,1}));
    assert((filters == std::vector<uint8_t>{1,1,1}));
    assert(!controller.scan_paused_);
    assert(stats.pause_attempts.load() == 1);
    assert(stats.disable_confirmed.load() == 1);
    assert(stats.restore_attempts.load() == 1);
    assert(stats.restore_confirmed.load() == 1);
    reject_send=true;
    assert(!hci.pause_scan(hci.context));
    reject_send=false;
    completion_status=0x0c;
    hci.resume_scan(hci.context);
    assert(controller.scan_paused_); // Rejected restore remains recoverable.
    completion_status=0;
    hci.resume_scan(hci.context);
    assert(!controller.scan_paused_);
    defer_completion=true;
    assert(!hci.pause_scan(hci.context));
    assert(controller.tls_scan_completion_.load() == 4);
    assert(stats.wait_timeouts.load() == 1);
    assert(stats.rejected.load() == 3);
    assert(!hci.send(hci.context,enable,sizeof(enable))); // No opcode collision.
    const int events_before_late = host_events;
    defer_completion=false;
    vTaskDelay(1); // Late disable triggers a private restore.
    assert(controller.tls_scan_completion_.load() == 5);
    vTaskDelay(1); // Restore completion is also private.
    assert(!controller.scan_paused_);
    assert(host_events == events_before_late);
    assert(stats.disable_confirmed.load() == 2);
    assert(stats.restore_confirmed.load() == 3);
    assert(hci.pause_scan(hci.context));
    hci.resume_scan(hci.context);
    hci.stop(hci.context);
    assert(!hci.pause_scan(hci.context));
}
''')
    output = work / "test"
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-I" + str(work), "-I" + str(APP), str(work / "test.cpp"),
        str(APP / "bouffalo_hci_controller.cpp"),
        str(APP / "bouffalo_hci_packet_adapter.cpp"), "-o", str(output)], check=True)
    subprocess.run([str(output)], check=True)
print("BL616CL_TLS_SCAN_WINDOW_PASS")
