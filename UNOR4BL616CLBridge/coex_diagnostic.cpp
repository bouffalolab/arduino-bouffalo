#include "coex_diagnostic.h"
#include <FreeRTOS.h>
#include <task.h>
#include <string.h>
extern "C" {
#include "wifi_mgmr_ext.h"
#include "macsw_config.h"
#include "btble_lib_api.h"
}

namespace bridge {
extern "C" void bridge_tcp_listen_observer(unsigned event, unsigned port)
{
    auto &counters = tcp_listen_ingress_counters();
    if (event >= 6U) return;
    ++counters.events[event];
    counters.last_port.store(port);
}
// Passive SDK hooks: lifetime sequential fields, no MAC addresses or mutation.
extern "C" void bridge_ap_sta_remove_observer(uintptr_t site)
{
    auto &counters = ap_station_removal_counters();
    ++counters.removes;
    counters.remove_site.store(static_cast<uint32_t>(site));
}
extern "C" void bridge_ap_sta_free_observer(uintptr_t site, unsigned timeout_next,
                                            unsigned flags)
{
    auto &counters = ap_station_removal_counters();
    counters.free_site.store(static_cast<uint32_t>(site));
    counters.timeout_next.store(timeout_next);
    counters.flags.store(flags);
}
extern "C" void bridge_ap_sta_rx_leave_observer(unsigned kind, unsigned reason)
{
    auto &counters = ap_station_removal_counters();
    counters.rx_leave_kind.store(kind);
    counters.rx_leave_reason.store(reason);
}
#if defined(BL616CL_AP_SCAN_PTI)
static_assert(BL616CL_AP_SCAN_PTI >= 0 && BL616CL_AP_SCAN_PTI <= 15,
              "AP BLE scan PTI must fit the controller priority field");
namespace {
uint32_t saved_active_scan_priority = 0U;
uint32_t saved_passive_scan_priority = 0U;
bool scan_priority_overridden = false;
}
#endif

void coex_apply_ap_scan_priority(bool ap_start_confirmed)
{
#if defined(BL616CL_AP_SCAN_PTI)
    if (!ap_start_confirmed && !wifi_mgmr_ap_state_get()) return;
    if (!scan_priority_overridden) {
        saved_active_scan_priority = btble_controller_get_event_priority(BLE_EVENT_ACTIVE_SCANNING);
        saved_passive_scan_priority = btble_controller_get_event_priority(BLE_EVENT_PASSIVE_SCANNING);
        if (saved_active_scan_priority > 15U || saved_passive_scan_priority > 15U) return;
    }
    (void)btble_controller_set_event_priority(BLE_EVENT_ACTIVE_SCANNING, BL616CL_AP_SCAN_PTI);
    (void)btble_controller_set_event_priority(BLE_EVENT_PASSIVE_SCANNING, BL616CL_AP_SCAN_PTI);
    scan_priority_overridden = true;
#else
    (void)ap_start_confirmed;
#endif
}

void coex_restore_ap_scan_priority()
{
#if defined(BL616CL_AP_SCAN_PTI)
    if (scan_priority_overridden) {
        (void)btble_controller_set_event_priority(BLE_EVENT_ACTIVE_SCANNING, saved_active_scan_priority);
        (void)btble_controller_set_event_priority(BLE_EVENT_PASSIVE_SCANNING, saved_passive_scan_priority);
        scan_priority_overridden = false;
    }
#endif
}

void coex_apply_ap_join_scan_priority()
{
#if defined(BL616CL_AP_JOIN_SCAN_PTI)
    static_assert(BL616CL_AP_JOIN_SCAN_PTI >= 0 && BL616CL_AP_JOIN_SCAN_PTI <= 15,
                  "experimental AP join scan priority must fit four bits");
    // Preserve the normal AP override's original priorities for AP teardown.
    coex_apply_ap_scan_priority(true);
    (void)btble_controller_set_event_priority(BLE_EVENT_ACTIVE_SCANNING, BL616CL_AP_JOIN_SCAN_PTI);
    (void)btble_controller_set_event_priority(BLE_EVENT_PASSIVE_SCANNING, BL616CL_AP_JOIN_SCAN_PTI);
#endif
}

#if defined(BL616CL_AP_BEACON_PTI)
static_assert(BL616CL_AP_BEACON_PTI >= 0 && BL616CL_AP_BEACON_PTI <= 15,
              "AP beacon PTI must fit the SDK four-bit field");
namespace {
uint32_t saved_beacon_priority = 0U;
bool beacon_priority_overridden = false;
}
#endif

void coex_apply_ap_beacon_priority()
{
#if defined(BL616CL_AP_BEACON_PTI)
    // Pinned SDK reg_mac_core.h: COEX_PTI_BCN_DATA [31:28], 0x24B00404.
    // Experimental A/B option; preserve every other traffic priority.
    auto &pti = *reinterpret_cast<volatile uint32_t *>(0x24b00404U);
    taskENTER_CRITICAL();
    if (!beacon_priority_overridden) saved_beacon_priority = pti & 0xf0000000U;
    pti = (pti & 0x0fffffffU) | (static_cast<uint32_t>(BL616CL_AP_BEACON_PTI) << 28U);
    beacon_priority_overridden = true;
    taskEXIT_CRITICAL();
#endif
}

void coex_restore_ap_beacon_priority()
{
#if defined(BL616CL_AP_BEACON_PTI)
    auto &pti = *reinterpret_cast<volatile uint32_t *>(0x24b00404U);
    taskENTER_CRITICAL();
    if (beacon_priority_overridden) pti = (pti & 0x0fffffffU) | saved_beacon_priority;
    beacon_priority_overridden = false;
    taskEXIT_CRITICAL();
#endif
}

CoexDiagnostic coex_diagnostic_snapshot()
{
    // Match the pinned SDK's macsw/plf/refip/src/driver/bl616cl.c, not the
    // BL616/BL618DG map (0x24920028 is a read-only statistic on BL616CL).
    const auto read = [](uintptr_t address) {
        return *reinterpret_cast<volatile const uint32_t *>(address);
    };
    CoexDiagnostic snapshot = {read(0x24920004U), read(0x24b00400U), read(0x24b00404U),
            read(0x24b00418U), read(0x24b00410U), read(0x24920020U),
            read(0x24920024U), 0U, 0U, 0U, 0U, 0U};
    uint8_t destination[6] = {};
    uint8_t source[6] = {};
    taskENTER_CRITICAL();
    memcpy(destination, wifi_syn_counters().last_discard_destination, sizeof(destination));
    memcpy(source, wifi_syn_counters().last_discard_source, sizeof(source));
    const bool has_discard = wifi_syn_counters().last_discard_site.load() != 0U;
    taskEXIT_CRITICAL();
    snapshot.ap_state = static_cast<uint32_t>(wifi_mgmr_ap_state_get());
    snapshot.scan_priorities =
        (btble_controller_get_event_priority(BLE_EVENT_ACTIVE_SCANNING) << 4U) |
        btble_controller_get_event_priority(BLE_EVENT_PASSIVE_SCANNING);
    uint8_t mac[6] = {};
    if (has_discard && wifi_mgmr_sta_mac_get(mac) == 0 && memcmp(source, mac, sizeof(mac)) == 0) {
        snapshot.discarded_source_interface |= 1U;
    }
    if (has_discard && wifi_mgmr_ap_mac_get(mac) == 0 && memcmp(source, mac, sizeof(mac)) == 0) {
        snapshot.discarded_source_interface |= 2U;
    }
    // Query the manager outside the critical section: its accessor takes a mutex.
    if (snapshot.ap_state != 0U) {
        for (uint8_t index = 0U; index < CFG_STA_MAX; ++index) {
            wifi_sta_basic_info station = {};
            if (wifi_mgmr_ap_sta_info_get(&station, index) == 0 && station.is_used) {
                ++snapshot.ap_stations;
                if (has_discard && memcmp(destination, station.sta_mac, sizeof(destination)) == 0) {
                    ++snapshot.discarded_destination_matches_station;
                }
            }
        }
    }
    return snapshot;
}
}
