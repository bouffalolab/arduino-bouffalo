#pragma once

#include <stdint.h>
#include <atomic>

namespace bridge {
struct ApJoinScanCounters {
    std::atomic<uint32_t> active{0};
    std::atomic<uint32_t> attempts{0};
    std::atomic<uint32_t> dhcp_ack_transmitted{0};
    std::atomic<uint32_t> completed{0};
    std::atomic<uint32_t> timeouts{0};
    std::atomic<uint32_t> elapsed_ms{0};
    std::atomic<uint32_t> client_ip_reply_acknowledged{0};
    std::atomic<uint32_t> ap_ipv4{0};
};
inline ApJoinScanCounters &ap_join_scan_counters()
{
    static ApJoinScanCounters counters;
    return counters;
}
struct TcpListenIngressCounters {
    std::atomic<uint32_t> events[6]{};
    std::atomic<uint32_t> last_port{0};
};
inline TcpListenIngressCounters &tcp_listen_ingress_counters()
{
    static TcpListenIngressCounters counters;
    return counters;
}
struct ApStationRemovalCounters {
    std::atomic<uint32_t> removes{0};
    std::atomic<uint32_t> remove_site{0};
    std::atomic<uint32_t> free_site{0};
    std::atomic<uint32_t> timeout_next{0};
    std::atomic<uint32_t> flags{0};
    std::atomic<uint32_t> rx_leave_kind{0}; // 1: received disassoc, 2: received deauth.
    std::atomic<uint32_t> rx_leave_reason{0};
};
inline ApStationRemovalCounters &ap_station_removal_counters()
{
    static ApStationRemovalCounters counters;
    return counters;
}
struct WifiSynCounters {
    std::atomic<uint32_t> confirmed{0};
    std::atomic<uint32_t> acknowledged{0};
    std::atomic<uint32_t> discarded{0};
    std::atomic<uint32_t> last_discard_site{0};
    uint8_t last_discard_destination[6] = {}; // Protected by task critical section.
    uint8_t last_discard_source[6] = {};
};
inline WifiSynCounters &wifi_syn_counters()
{
    static WifiSynCounters counters;
    return counters;
}
// Lifetime counters, shared by the AT task and controller callback. Reading
// several counters is passive and does not provide an atomic group snapshot.
struct ScanWindowCounters {
    std::atomic<uint32_t> pause_attempts{0};
    std::atomic<uint32_t> disable_confirmed{0};
    std::atomic<uint32_t> restore_attempts{0};
    std::atomic<uint32_t> restore_confirmed{0};
    std::atomic<uint32_t> rejected{0};
    std::atomic<uint32_t> wait_timeouts{0};
};
inline ScanWindowCounters &scan_window_counters()
{
    static ScanWindowCounters counters;
    return counters;
}
// Read-only BL616CL snapshots. Counters are RF requests/aborts, not delivered
// network packets; sequential MMIO reads do not form an atomic RF snapshot.
struct CoexDiagnostic {
    uint32_t pta_config;
    uint32_t wlan_control;
    uint32_t wlan_pti;
    uint32_t wlan_pti2;
    uint32_t wlan_control2;
    uint32_t wlan_rx;
    uint32_t wlan_rx_abort;
    uint32_t ap_state;
    uint32_t ap_stations;
    uint32_t discarded_destination_matches_station;
    uint32_t discarded_source_interface; // Bit 0: STA MAC, bit 1: AP MAC.
    uint32_t scan_priorities; // Active in bits 7:4, passive in bits 3:0.
};
CoexDiagnostic coex_diagnostic_snapshot();
void coex_apply_ap_beacon_priority();
void coex_restore_ap_beacon_priority();
void coex_apply_ap_scan_priority(bool ap_start_confirmed = false);
void coex_apply_ap_join_scan_priority();
void coex_restore_ap_scan_priority();
}
