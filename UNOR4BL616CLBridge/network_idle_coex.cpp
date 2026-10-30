#include "network_idle_coex.h"
#if BL616CL_NETWORK_IDLE_COEX
#include <Arduino.h>
#include "bouffalo_hci_controller.h"
#include "bouffalo_wifi_backend.h"
#include "network_scan_window.h"
#endif
namespace bridge {
void network_idle_coex_poll() {
#if BL616CL_NETWORK_IDLE_COEX
    // Incoming network packets have no RA4 command to request an RF window.
    // Run only in the AT task after runtime initialization, even when the
    // optional native IPv6 diagnostic listener is not compiled in.
    static uint32_t next = 0;
    static uint32_t last_window = 0;
    const uint32_t now = millis();
    const uint32_t current_window = NetworkScanWindow::activity_generation();
    if (current_window != last_window) {
        last_window = current_window;
        // Existing network commands already paused BLE for WLAN. Avoid an
        // extra 80ms pause across their cached-available/read/response cycle.
        // Idle receive still receives periodic windows when these commands stop.
        next = now + 100U;
        return;
    }
    if (static_cast<int32_t>(now - next) < 0) return;
    const bool ap_ready = bouffalo_wifi_ap_ready();
    if (ap_ready) {
        auto controller = BouffaloHciController::active_controller();
        NetworkScanWindow window(controller.pause_scan, controller.resume_scan,
                                 controller.context);
        if (window.paused()) delay(40);
    }
    last_window = NetworkScanWindow::activity_generation();
    // pause/resume each already gives WLAN 20ms. Leave at least 40ms of
    // restored BLE time before asking again; preserve the requested BLE scan parameters.
    next = millis() + 40U;
#endif
}
}
