#ifndef UNOR4_BL616CL_WIFI_SERVICE_H
#define UNOR4_BL616CL_WIFI_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "at_transport.h"
#include "wifi_status.h"

namespace bridge {

constexpr size_t kWifiScanMaxResults = 50U;
constexpr size_t kWifiScanSsidBytes = 32U;
constexpr size_t kWifiStaSsidBytes = 32U;
constexpr size_t kWifiStaPasswordBytes = 64U;
constexpr size_t kWifiApSsidBytes = 32U;
constexpr size_t kWifiApPasswordBytes = 64U;
constexpr size_t kWifiHostnameBytes = 31U;
constexpr size_t kWifiIpv6TextBytes = 40U;

struct WifiApNetworkConfig {
    bool configured = false;
    uint8_t ip[4] = {};
    uint8_t gateway[4] = {};
    uint8_t netmask[4] = {};
};

struct WifiStaNetworkConfig {
    /*
     * A nonzero IPv4 configuration selects static addressing. DNS can also
     * be overridden independently while preserving a current DHCP lease:
     * WiFiS3::setDNS() serializes through +SETIP using that lease's IP,
     * gateway, and netmask.
     */
    bool configured = false;
    bool dns_configured = false;
    uint8_t ip[4] = {};
    uint8_t gateway[4] = {};
    uint8_t netmask[4] = {};
    uint8_t dns1[4] = {};
    uint8_t dns2[4] = {};
};

struct WifiStaIpInfo {
    uint8_t ip[4] = {};
    uint8_t gateway[4] = {};
    uint8_t netmask[4] = {};
    uint8_t dns1[4] = {};
    uint8_t dns2[4] = {};
};

struct WifiScanResult {
    uint8_t ssid[kWifiScanSsidBytes];
    uint8_t ssid_size;
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    uint8_t auth;
};

struct WifiBackend {
    void *context = nullptr;
    bool (*start_scan)(void *context) = nullptr;
    size_t (*copy_scan_results)(
        void *context,
        WifiScanResult *results,
        size_t result_capacity) = nullptr;
    bool (*start_sta_connect)(
        void *context,
        const uint8_t *ssid,
        size_t ssid_size,
        const uint8_t *password,
        size_t password_size,
        const WifiStaNetworkConfig *network,
        bool auto_reconnect) = nullptr;
    bool (*set_sta_dns)(
        void *context,
        const uint8_t dns1[4],
        const uint8_t dns2[4]) = nullptr;
    bool (*disconnect_sta)(void *context, bool wifi_off) = nullptr;
    bool (*set_auto_reconnect)(void *context, bool enabled) = nullptr;
    bool (*set_sta_role_enabled)(void *context, bool enabled) = nullptr;
    bool (*enable_sta_coexistence)(void *context) = nullptr;
    bool (*get_sta_ssid)(
        void *context,
        uint8_t *ssid,
        size_t ssid_capacity,
        size_t *ssid_size) = nullptr;
    bool (*get_sta_rssi)(void *context, int *rssi) = nullptr;
    bool (*get_sta_bssid)(void *context, uint8_t bssid[6]) = nullptr;
    bool (*get_sta_ip_info)(void *context, WifiStaIpInfo *info) = nullptr;
    bool (*start_softap)(
        void *context,
        const uint8_t *ssid,
        size_t ssid_size,
        const uint8_t *password,
        size_t password_size,
        uint8_t channel,
        bool hidden,
        uint8_t max_connections,
        const WifiApNetworkConfig *network) = nullptr;
    bool (*stop_softap)(void *context, bool wifi_off) = nullptr;
    bool (*get_ap_mac)(void *context, uint8_t mac[6]) = nullptr;
    bool (*get_ap_ip)(
        void *context,
        uint8_t ip[4],
        uint8_t gateway[4],
        uint8_t netmask[4]) = nullptr;
    bool (*set_ap_config)(
        void *context,
        const uint8_t ip[4],
        const uint8_t gateway[4],
        const uint8_t netmask[4]) = nullptr;
    bool (*set_sta_hostname)(
        void *context,
        const uint8_t *hostname,
        size_t hostname_size) = nullptr;
    bool (*get_sta_hostname)(
        void *context,
        uint8_t *hostname,
        size_t hostname_capacity,
        size_t *hostname_size) = nullptr;
    bool (*enable_sta_ipv6)(void *context) = nullptr;
    bool (*get_sta_ipv6)(
        void *context,
        char *address,
        size_t address_capacity) = nullptr;
    bool (*get_sta_mac)(void *context, uint8_t mac[6]) = nullptr;
};

struct WifiWaitHooks {
    void *context = nullptr;
    uint32_t (*now_ms)(void *context) = nullptr;
    void (*idle)(void *context) = nullptr;
};

enum class WifiEventCode : uint16_t {
    InitDone = 1U,
    ManagerDone = 2U,
    Connected = 4U,
    Disconnected = 5U,
    GotIp = 7U,
    ScanDone = 9U,
    ApStarted = 11U,
    ApStopped = 12U,
    ApStationAdded = 21U,
    ApStationRemoved = 22U,
};

/*
 * Provides native Wi-Fi scan and minimal STA control while retaining the
 * ESP32-S3 AT command framing. Association success remains event-driven:
 * BEGINSTA reports the immediate status snapshot, not a completed join.
 *
 * The AT mode values select the user-visible STA/AP lifecycle. They remain
 * separate from Bouffalo's PHY-mode API: the latter selects b/g/n/ax modes.
 */
class WifiService {
public:
    static constexpr uint32_t kScanTimeoutMs = 20000U;
    static constexpr uint32_t kSoftApResponseGraceMs = 50U;

    WifiService(
        WifiStatusTracker &status_tracker,
        WifiBackend backend,
        WifiWaitHooks wait_hooks = {});

    bool handles_command(const at::Command &command) const;
    at::CommandResult handle_command(
        const at::Command &command,
        at::AtTransport &transport);

    bool start_scan();
    bool scan_active() const;
    bool scan_complete() const;
    size_t scan_result_count();
    const WifiScanResult *scan_results() const;
    bool poll();
    bool disconnect_for_bootstrap();
    void on_wifi_event(uint16_t code);

private:
    at::CommandResult handle_begin_sta(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_set_ip(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_wifi_mode(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_soft_reset_wifi(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_reconnect(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_sta_rssi(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_sta_ip(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_sta_ssid(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_sta_bssid(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_sta_mac(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_auto_connect(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_disconnect(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_begin_softap(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_disconnect_softap(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_mac_softap(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_ip_softap(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_softap_ssid(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_softap_config(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_hostname(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_ipv6(
        const at::Command &command,
        at::AtTransport &transport);
    bool wait_for_scan();
    bool scan_payload_size(size_t result_count, size_t &payload_size) const;
    bool write_scan_response(at::AtTransport &transport);
    bool write_status_response(
        const at::Command &command,
        at::AtTransport &transport,
        WifiStatus status) const;
    static const char *auth_text(uint8_t auth);
    static bool parse_esp32_bool(const char *text, bool &value);
    static bool parse_uint8(
        const char *text,
        unsigned int minimum,
        unsigned int maximum,
        uint8_t &value);
    static bool parse_ipv4(const char *text, uint8_t octets[4]);
    static bool write_ipv4(
        at::AtTransport &transport,
        const uint8_t octets[4]);
    bool ap_mode_enabled() const;
    bool sta_mode_enabled() const;
    bool sta_connection_active() const;

    WifiStatusTracker &status_tracker_;
    WifiBackend backend_;
    WifiWaitHooks wait_hooks_;
    WifiScanResult scan_results_[kWifiScanMaxResults] = {};
    size_t scan_result_count_ = 0U;
    volatile bool scan_active_ = false;
    volatile bool scan_complete_ = false;
    bool auto_reconnect_enabled_ = true;
    bool sta_connect_pending_ = false;
    bool sta_dns_restore_pending_ = false;
    bool sta_coexistence_pending_ = false;
    bool sta_coexistence_active_ = false;
    uint8_t wifi_mode_ = 1U;
    WifiStaNetworkConfig sta_network_config_;
    uint8_t ap_ssid_[kWifiApSsidBytes] = {};
    size_t ap_ssid_size_ = 0U;
    bool ap_started_ = false;
    WifiApNetworkConfig ap_network_config_;
    uint8_t pending_ap_ssid_[kWifiApSsidBytes] = {};
    size_t pending_ap_ssid_size_ = 0U;
    uint8_t pending_ap_password_[kWifiApPasswordBytes] = {};
    size_t pending_ap_password_size_ = 0U;
    uint8_t pending_ap_channel_ = 1U;
    bool pending_ap_hidden_ = false;
    uint8_t pending_ap_max_connections_ = 4U;
    WifiApNetworkConfig pending_ap_network_config_;
    bool ap_start_pending_ = false;
    uint32_t ap_start_not_before_ms_ = 0U;
};

} // namespace bridge

#endif
