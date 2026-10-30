#include "bouffalo_wifi_backend.h"
#include "bouffalo_hci_controller.h"
#include "network_scan_window.h"
#include "usb_cmsis_dap.h"
#include "wifi_ap_start_diagnostic.h"
#include "coex_diagnostic.h"

#include "Arduino.h"
#include "bridge_config.h"

#include <string.h>

extern "C" {
#include <FreeRTOS.h>
#include <task.h>
#include "wifi_mgmr.h"
#include "wifi_mgmr_ext.h"
#include "fhost.h"
/* fhost_wpa.h pulls private eloop headers outside the exported bridge SDK.
 * Declare these public FHOST entry points with their audited SDK signatures.
 * enum fhost_wpa_state defines STOPPED as zero.
 */
uint8_t fhost_wpa_get_state(int fhost_vif_idx);
int fhost_wpa_execute_cmd(int fhost_vif_idx, char *resp_buf, int *resp_buf_len,
                          const char *cmd, ...);
#include "net_al.h"
#include "net_al_ext.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/lwip/ip4_addr.h"
#include "lwip/ip6_addr.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
}

namespace bridge {

namespace {

struct SoftApStartRequest {
    char ssid[kWifiApSsidBytes + 1U] = {};
    char password[kWifiApPasswordBytes + 1U] = {};
    uint8_t password_size = 0U;
    uint8_t channel = 1U;
    bool hidden = false;
    uint8_t max_connections = 4U;
    uint32_t ap_ipaddr = 0U;
    uint32_t ap_mask = 0U;
};

volatile bool softap_start_active = false;
SoftApStartRequest softap_start_request;

/*
 * The FHOST WPA/ELOOP task runs at priority 26. AP start waits for that task
 * to complete synchronous control requests, so it must share that scheduling
 * class rather than inherit the Arduino loop's background priority of 1.
 * Keep Wi-Fi/RX/TX/IPC tasks above it at the SDK-assigned 27-30 range.
 */
constexpr UBaseType_t kSoftApStartTaskPriority = configMAX_PRIORITIES - 6U;
static_assert(kSoftApStartTaskPriority == 26U,
              "bridge SoftAP worker priority must match FHOST WPA");

uint32_t make_ip(const uint8_t octets[4])
{
    ip4_addr_t address;
    IP4_ADDR(&address, octets[0], octets[1], octets[2], octets[3]);
    return address.addr;
}

void copy_ip(uint32_t value, uint8_t octets[4])
{
    ip4_addr_t address = {value};
    octets[0] = ip4_addr1(&address);
    octets[1] = ip4_addr2(&address);
    octets[2] = ip4_addr3(&address);
    octets[3] = ip4_addr4(&address);
}

char sta_hostname[kWifiHostnameBytes + 1U] = "wlan";

bool set_sta_dns(
    void *,
    const uint8_t dns1[4],
    const uint8_t dns2[4])
{
    if (dns1 == nullptr || dns2 == nullptr) {
        return false;
    }

    ip_addr_t primary = {};
    ip_addr_t secondary = {};
    IP_ADDR4(&primary, dns1[0], dns1[1], dns1[2], dns1[3]);
    IP_ADDR4(&secondary, dns2[0], dns2[1], dns2[2], dns2[3]);
    LOCK_TCPIP_CORE();
    dns_setserver(0U, &primary);
    dns_setserver(1U, &secondary);
    UNLOCK_TCPIP_CORE();
    return true;
}

bool start_scan(void *)
{
    wifi_mgmr_scan_params_t parameters = {};
    return wifi_mgmr_sta_scan(&parameters) == 0;
}

size_t copy_scan_results(void *, WifiScanResult *results, size_t result_capacity)
{
    if (results == nullptr || result_capacity == 0U) {
        return 0U;
    }

    static wifi_mgmr_scan_item_t sdk_results[kWifiScanMaxResults] = {};
    const uint32_t requested =
        result_capacity < kWifiScanMaxResults ? result_capacity : kWifiScanMaxResults;
    const uint32_t copied = wifi_mgmr_sta_scanlist_dump(sdk_results, requested);
    const size_t result_count = copied < requested ? copied : requested;

    for (size_t index = 0U; index < result_count; ++index) {
        const wifi_mgmr_scan_item_t &source = sdk_results[index];
        WifiScanResult &destination = results[index];
        const size_t ssid_size =
            source.ssid_len > 0
            ? static_cast<size_t>(source.ssid_len)
            : 0U;
        destination.ssid_size =
            ssid_size < sizeof(destination.ssid) ? ssid_size : sizeof(destination.ssid);
        memcpy(destination.ssid, source.ssid, destination.ssid_size);
        memcpy(destination.bssid, source.bssid, sizeof(destination.bssid));
        destination.rssi = source.rssi;
        destination.channel = source.channel;
        destination.auth = source.auth;
    }
    return result_count;
}

bool start_sta_connect(
    void *,
    const uint8_t *ssid,
    size_t ssid_size,
    const uint8_t *password,
    size_t password_size,
    const WifiStaNetworkConfig *network,
    bool auto_reconnect)
{
    if (ssid == nullptr || ssid_size == 0U || ssid_size > MGMR_SSID_LEN ||
        password_size > MGMR_KEY_LEN ||
        (password_size != 0U && password == nullptr)) {
        return false;
    }

    const bool static_ipv4 = network != nullptr && network->configured;
    if (static_ipv4) {
        if (wifi_mgmr_sta_ip_set(
                make_ip(network->ip),
                make_ip(network->netmask),
                make_ip(network->gateway),
                make_ip(network->dns1)) != 0) {
            return false;
        }
        if (!set_sta_dns(nullptr, network->dns1, network->dns2)) {
            return false;
        }
    }

    wifi_mgmr_sta_connect_params_t parameters = {};
    memcpy(parameters.ssid, ssid, ssid_size);
    parameters.ssid_len = static_cast<uint8_t>(ssid_size);
    if (password_size != 0U) {
        memcpy(parameters.key, password, password_size);
        parameters.key_len = static_cast<uint8_t>(password_size);
    }
    /*
     * For DHCP, fhost creates the STA VIF asynchronously and starts the
     * client from its WPA-connected task after association. Starting DHCP
     * before wifi_mgmr_sta_connect() can race that VIF lifecycle and leave
     * the bridge's AT task blocked. Static IPv4 remains installed first and
     * tells fhost not to start DHCP.
     */
    parameters.use_dhcp = static_ipv4 ? 0U : 1U;
    parameters.pmf_cfg = 1U;

    /*
     * The SDK's public autoconnect setters print to stdout even before a STA
     * VIF exists. stdout is not a bridge protocol channel, so update the
     * policy consumed by fhost_wpa_add_vif() directly; that function applies
     * STA_AUTOCONNECT when it creates the VIF.
     */
    wifiMgmr.disable_autoreconnect = auto_reconnect ? 0U : 1U;
    if (wifi_mgmr_sta_connect(&parameters) != 0) {
        return false;
    }

    /*
     * A STA netif may be created by fhost during the connection request.
     * Reapply the stable hostname after that lifecycle point so DHCP sees
     * the user-selected name rather than the SDK's default "wlan".
     */
    LOCK_TCPIP_CORE();
    struct netif *netif =
        static_cast<struct netif *>(fhost_to_net_if(MGMR_VIF_STA));
    if (netif != nullptr) {
        netif_set_hostname(netif, sta_hostname);
    }
    UNLOCK_TCPIP_CORE();
    return true;
}

bool disconnect_sta(void *, bool wifi_off)
{
    if (wifi_off) {
        /*
         * The bridge bundle has the native STA disconnect API but no audited
         * radio power-down lifecycle. Do not report DISCONNECT=1 as success
         * until WIFIMODE/radio lifecycle support is implemented.
         */
        return false;
    }
    return wifi_sta_disconnect() == 0;
}

bool set_auto_reconnect(void *, bool enabled)
{
    /* Keep both the next-VIF default and the live supplicant in sync. The
     * SDK public setters also issue STA_AUTOCONNECT, but print to the shared
     * AT UART. Use their native control operation without that status text.
     */
    const uint8_t previous = wifiMgmr.disable_autoreconnect;
    wifiMgmr.disable_autoreconnect = enabled ? 0U : 1U;
    if (fhost_wpa_get_state(MGMR_VIF_STA) != 0U &&
        fhost_wpa_execute_cmd(MGMR_VIF_STA, nullptr, nullptr,
                              "STA_AUTOCONNECT %u", enabled ? 1U : 0U) != 0) {
        wifiMgmr.disable_autoreconnect = previous;
        return false;
    }
    return true;
}

bool set_sta_role_enabled(void *, bool enabled)
{
    if (enabled) {
        /*
         * fhost is initialized once by BridgeRuntime. A STA VIF is created
         * by wifi_mgmr_sta_connect(), so enabling the AT STA role only makes
         * that lifecycle available again.
         */
        return true;
    }

    wifiMgmr.disable_autoreconnect = 1U;
    if (!wifi_mgmr_sta_state_get()) {
        return true;
    }
    return wifi_sta_disconnect() == 0;
}

bool enable_sta_coexistence(void *)
{
    return kEnableStaPsPtaCoexistence && wifi_mgmr_sta_coex_enable() == 0;
}

bool get_sta_ssid(
    void *,
    uint8_t *ssid,
    size_t ssid_capacity,
    size_t *ssid_size)
{
    if (ssid == nullptr || ssid_size == nullptr ||
        ssid_capacity == 0U ||
        !wifi_mgmr_sta_state_get()) {
        return false;
    }

    char ssid_text[kWifiStaSsidBytes + 1U] = {};
    if (wifi_mgmr_sta_ssid_get(ssid_text) != 0) {
        return false;
    }
    const size_t size = strnlen(ssid_text, sizeof(ssid_text));
    if (size == 0U || size == sizeof(ssid_text) || size > ssid_capacity) {
        return false;
    }
    memcpy(ssid, ssid_text, size);
    *ssid_size = size;
    return true;
}

bool get_sta_rssi(void *, int *rssi)
{
    return rssi != nullptr &&
        wifi_mgmr_sta_state_get() &&
        wifi_mgmr_sta_rssi_get(rssi) == 0;
}

bool get_sta_bssid(void *, uint8_t bssid[6])
{
    return bssid != nullptr &&
        wifi_mgmr_sta_state_get() &&
        wifi_mgmr_sta_get_bssid(bssid) == 0;
}

bool get_sta_ip_info(void *, WifiStaIpInfo *info)
{
    if (info == nullptr || !wifi_mgmr_sta_state_get()) {
        return false;
    }

    net_al_ext_ip_addr_cfg config = {};
    if (net_al_ext_get_vif_ip(MGMR_VIF_STA, &config) != 0) {
        return false;
    }

    copy_ip(config.ipv4.addr, info->ip);
    copy_ip(config.ipv4.gw, info->gateway);
    copy_ip(config.ipv4.mask, info->netmask);
    copy_ip(config.ipv4.dns, info->dns1);
    LOCK_TCPIP_CORE();
    const ip_addr_t *dns1 = dns_getserver(0U);
    const ip_addr_t *dns2 = dns_getserver(1U);
    if (dns1 != nullptr && IP_IS_V4(dns1)) {
        copy_ip(ip_2_ip4(dns1)->addr, info->dns1);
    }
    if (dns2 != nullptr && IP_IS_V4(dns2)) {
        copy_ip(ip_2_ip4(dns2)->addr, info->dns2);
    }
    UNLOCK_TCPIP_CORE();
    return true;
}

bool set_sta_hostname(
    void *,
    const uint8_t *hostname,
    size_t hostname_size)
{
    if (hostname == nullptr || hostname_size == 0U ||
        hostname_size > kWifiHostnameBytes) {
        return false;
    }

    memcpy(sta_hostname, hostname, hostname_size);
    sta_hostname[hostname_size] = '\0';

    LOCK_TCPIP_CORE();
    struct netif *netif =
        static_cast<struct netif *>(fhost_to_net_if(MGMR_VIF_STA));
    if (netif != nullptr) {
        netif_set_hostname(netif, sta_hostname);
    }
    UNLOCK_TCPIP_CORE();
    return true;
}

bool get_sta_hostname(
    void *,
    uint8_t *hostname,
    size_t hostname_capacity,
    size_t *hostname_size)
{
    if (hostname == nullptr || hostname_size == nullptr ||
        hostname_capacity == 0U) {
        return false;
    }

    LOCK_TCPIP_CORE();
    const size_t size = strnlen(sta_hostname, kWifiHostnameBytes + 1U);
    const bool valid = size != 0U &&
        size <= kWifiHostnameBytes &&
        size <= hostname_capacity;
    if (valid) {
        memcpy(hostname, sta_hostname, size);
        *hostname_size = size;
    }
    UNLOCK_TCPIP_CORE();
    return valid;
}

bool enable_sta_ipv6(void *)
{
#ifdef CFG_IPV6
    return wifi_sta_ipv6_enable(1) == 0;
#else
    return false;
#endif
}

bool get_sta_ipv6(void *, char *address, size_t address_capacity)
{
#if defined(CFG_IPV6) && LWIP_IPV6
    if (address == nullptr || address_capacity < 2U) {
        return false;
    }

    bool found = false;
    LOCK_TCPIP_CORE();
    const struct netif *netif =
        static_cast<const struct netif *>(fhost_to_net_if(MGMR_VIF_STA));
    if (netif != nullptr) {
        for (uint8_t index = 0U; index < LWIP_IPV6_NUM_ADDRESSES; ++index) {
            const ip6_addr_t *candidate = netif_ip6_addr(netif, index);
            if (candidate != nullptr &&
                (netif_ip6_addr_state(netif, index) & IP6_ADDR_VALID) != 0U &&
                ip6_addr_islinklocal(candidate)) {
                found = ip6addr_ntoa_r(
                            candidate,
                            address,
                            static_cast<int>(address_capacity)) != nullptr;
                break;
            }
        }
    }
    UNLOCK_TCPIP_CORE();
    return found && address[0] != '\0';
#else
    (void)address;
    (void)address_capacity;
    return false;
#endif
}

void set_default_ap_network(WifiApNetworkConfig &network)
{
    network.configured = true;
    network.ip[0] = 192U;
    network.ip[1] = 168U;
    network.ip[2] = 4U;
    network.ip[3] = 1U;
    network.gateway[0] = 192U;
    network.gateway[1] = 168U;
    network.gateway[2] = 4U;
    network.gateway[3] = 1U;
    network.netmask[0] = 255U;
    network.netmask[1] = 255U;
    network.netmask[2] = 255U;
    network.netmask[3] = 0U;
}

bool set_ap_config(
    void *,
    const uint8_t ip[4],
    const uint8_t gateway[4],
    const uint8_t netmask[4]);

void hold_ap_start_scan_window(bool paused)
{
#if defined(BL616CL_AP_START_SCAN_WINDOW_MS)
    static_assert(BL616CL_AP_START_SCAN_WINDOW_MS >= 0 &&
                  BL616CL_AP_START_SCAN_WINDOW_MS <= 10000,
                  "experimental AP scan window must be bounded to ten seconds");
    if (paused && BL616CL_AP_START_SCAN_WINDOW_MS != 0)
        vTaskDelay(pdMS_TO_TICKS(BL616CL_AP_START_SCAN_WINDOW_MS));
#else
    (void)paused;
#endif
}

void finish_ap_join_scan_window(bool join_window, uint32_t join_reply_before)
{
#if defined(BL616CL_AP_JOIN_SCAN_TIMEOUT_MS)
    static_assert(BL616CL_AP_JOIN_SCAN_TIMEOUT_MS > 0 &&
                  BL616CL_AP_JOIN_SCAN_TIMEOUT_MS <= 10000,
                  "experimental AP join window must be bounded to ten seconds");
    const uint32_t timeout_ms = BL616CL_AP_JOIN_SCAN_TIMEOUT_MS;
#else
    const uint32_t timeout_ms = 5000U;
#endif
    if (join_window) {
        const uint32_t started_at = millis();
        while (ap_join_scan_counters().client_ip_reply_acknowledged.load() == join_reply_before &&
               millis() - started_at < timeout_ms) {
            vTaskDelay(pdMS_TO_TICKS(20U));
        }
        ap_join_scan_counters().elapsed_ms = millis() - started_at;
        if (ap_join_scan_counters().client_ip_reply_acknowledged.load() != join_reply_before)
            ++ap_join_scan_counters().completed;
        else
            ++ap_join_scan_counters().timeouts;
        BouffaloHciController::apply_ap_scan_priority_if_active();
    }
    ap_join_scan_counters().active = 0U;
}

void softap_start_task(void *)
{
#if defined(BL616CL_AP_JOIN_SCAN_PAUSE) && !defined(BL616CL_AP_JOIN_SCAN_PTI)
#error "experimental adaptive AP scan pause requires AP join diagnostics"
#endif
    /*
     * The SDK AP path contains synchronous WPA/FHOST waits. Keep it outside
     * the AT worker so a native stall cannot block USB, recovery, or later
     * diagnostic commands. Give the AT task one scheduler turn to enqueue
     * its accepted response before native FHOST tasks begin running.
     */
    SoftApStartRequest request = softap_start_request;
    vTaskDelay(pdMS_TO_TICKS(25U));
    bool join_window = false;
    uint32_t join_reply_before = 0U;
    {
#if defined(BL616CL_AP_START_SCAN_WINDOW_MS) || defined(BL616CL_AP_JOIN_SCAN_PTI)
        const auto hci = BouffaloHciController::active_controller();
#else
        const BleHciController hci = {};
#endif
        NetworkScanWindow scan_window(hci.pause_scan, hci.resume_scan, hci.context);
#if defined(BL616CL_AP_JOIN_SCAN_PTI)
#if !defined(BL616CL_AP_SCAN_PTI)
#error "experimental AP join priority requires a steady AP scan priority"
#endif
#if defined(BL616CL_AP_START_SCAN_WINDOW_MS)
        static_assert(BL616CL_AP_START_SCAN_WINDOW_MS == 0,
                      "AP join priority must not combine with an extra scan pause");
#endif
        if (scan_window.paused()) {
            join_reply_before = ap_join_scan_counters().client_ip_reply_acknowledged.load();
            ap_join_scan_counters().ap_ipv4 = request.ap_ipaddr;
            ap_join_scan_counters().active = 1U;
            ++ap_join_scan_counters().attempts;
        }
#endif
        wifi_ap_start_diagnostic_mark_native_call();
        wifi_ap_start_capture_uart_before_watchdog(
            Serial,
            Serial1,
            usb_cmsis_dap_current_line_coding_baud());
        wifi_ap_start_watchdog_arm();

        wifi_mgmr_ap_params_t parameters = {};
        parameters.ssid = request.ssid;
        parameters.key = request.password_size == 0U ? nullptr : request.password;
        parameters.akm = nullptr;
        parameters.channel = request.channel;
        parameters.use_ipcfg = true;
        parameters.use_dhcpd = true;
        parameters.start = 100;
        parameters.limit = 100;
        parameters.ap_ipaddr = request.ap_ipaddr;
        parameters.ap_mask = request.ap_mask;
        parameters.hidden_ssid = request.hidden;
        parameters.bcn_mode = 2U;

        const int native_result = wifi_mgmr_ap_start(&parameters);
        wifi_ap_start_watchdog_disarm();
        wifi_ap_start_diagnostic_complete(native_result);
        if (native_result == 0) {
            coex_apply_ap_beacon_priority();
            // BLE may already be scanning when AP becomes active. The HCI
            // scan-enable hook cannot apply AP policy while the AP is absent.
            BouffaloHciController::apply_ap_scan_priority_if_active();
#if defined(BL616CL_AP_JOIN_SCAN_PTI)
            if (scan_window.paused()) {
                join_window = BouffaloHciController::apply_ap_join_scan_priority_if_active();
            }
#endif
            (void)wifi_mgmr_conf_max_sta(request.max_connections);
            // Optional diagnostic RF window for initial association/DHCP.
            // This worker sleeps; AT, USB and recovery remain responsive.
            hold_ap_start_scan_window(scan_window.paused());
        }

#if defined(BL616CL_AP_JOIN_SCAN_PAUSE)
        // Keep the RF window through client IP traffic, rather than resuming
        // at association before DHCP finishes. The wait remains bounded.
        finish_ap_join_scan_window(join_window, join_reply_before);
        join_window = false;
#endif
    } // Restore scanning before deleting the FreeRTOS task.
    finish_ap_join_scan_window(join_window, join_reply_before);
    softap_start_active = false;
    vTaskDelete(nullptr);
}

bool start_softap(
    void *,
    const uint8_t *ssid,
    size_t ssid_size,
    const uint8_t *password,
    size_t password_size,
    uint8_t channel,
    bool hidden,
    uint8_t max_connections,
    const WifiApNetworkConfig *network)
{
    if (ssid == nullptr || ssid_size == 0U || ssid_size > kWifiApSsidBytes ||
        password_size > 63U || (password_size != 0U && password == nullptr) ||
        network == nullptr) {
        return false;
    }

    char ssid_text[kWifiApSsidBytes + 1U] = {};
    char password_text[kWifiApPasswordBytes + 1U] = {};
    memcpy(ssid_text, ssid, ssid_size);
    if (password_size != 0U) {
        memcpy(password_text, password, password_size);
    }

    WifiApNetworkConfig effective_network = *network;
    if (!effective_network.configured) {
        set_default_ap_network(effective_network);
    }

    if (softap_start_active) {
        return false;
    }

    /*
     * wifi_mgmr_ap_start() snapshots the existing AP VIF network mode before
     * it recreates that VIF. Restore the proven static-IP setup before the
     * asynchronous native start so the first AP instance keeps SOFTAPCONFIG
     * instead of falling back to the SDK DHCP default.
     */
    net_al_ext_ip_addr_cfg ip_config = {};
    ip_config.mode = IP_ADDR_STATIC_IPV4;
    ip_config.default_output = false;
    ip_config.ipv4.addr = make_ip(effective_network.ip);
    ip_config.ipv4.gw = make_ip(effective_network.gateway);
    ip_config.ipv4.mask = make_ip(effective_network.netmask);
    if (net_al_ext_set_vif_ip(MGMR_VIF_AP, &ip_config) != 0) {
        return false;
    }

    memcpy(softap_start_request.ssid, ssid_text, sizeof(softap_start_request.ssid));
    memcpy(softap_start_request.password, password_text,
           sizeof(softap_start_request.password));
    softap_start_request.password_size = static_cast<uint8_t>(password_size);
    softap_start_request.channel = channel;
    softap_start_request.hidden = hidden;
    softap_start_request.max_connections = max_connections;
    softap_start_request.ap_ipaddr = make_ip(effective_network.ip);
    softap_start_request.ap_mask = make_ip(effective_network.netmask);

    (void)wifi_mgmr_conf_max_sta(max_connections);
    wifi_ap_start_diagnostic_begin(channel);
    softap_start_active = true;
    if (xTaskCreate(
            softap_start_task,
            "softap_start",
            3072U,
            nullptr,
            kSoftApStartTaskPriority,
            nullptr) != pdPASS) {
        softap_start_active = false;
        wifi_ap_start_diagnostic_complete(-1);
        return false;
    }

    return true;
}

__attribute__((noinline)) bool stop_softap(void *, bool wifi_off)
{
    wifi_ap_start_diagnostic_record_bridge_stop_softap_caller(
        static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(__builtin_return_address(0))));
    if (softap_start_active) {
        /*
         * A native AP start may still be waiting inside FHOST. Do not call
         * another synchronous lifecycle operation from the AT worker; the
         * next bridge reset will discard the in-flight request.
         */
        return !wifi_off;
    }
    const bool stopped = wifi_mgmr_ap_stop() == 0;
    if (stopped) {
        coex_restore_ap_beacon_priority();
        coex_restore_ap_scan_priority();
    }
    /*
     * The current bundle does not expose a radio-wide power-down lifecycle.
     * Complete the AP stop, but report DISCONNECTSOFTAP=1 as unsupported.
     */
    return stopped && !wifi_off;
}

bool get_ap_mac(void *, uint8_t mac[6])
{
    return mac != nullptr && wifi_mgmr_ap_mac_get(mac) == 0;
}

bool get_sta_mac(void *, uint8_t mac[6])
{
    return mac != nullptr && wifi_mgmr_sta_mac_get(mac) == 0;
}

bool get_ap_ip(
    void *,
    uint8_t ip[4],
    uint8_t gateway[4],
    uint8_t netmask[4])
{
    if (ip == nullptr || gateway == nullptr || netmask == nullptr) {
        return false;
    }
    net_al_ext_ip_addr_cfg config = {};
    if (net_al_ext_get_vif_ip(MGMR_VIF_AP, &config) != 0) {
        return false;
    }
    copy_ip(config.ipv4.addr, ip);
    copy_ip(config.ipv4.gw, gateway);
    copy_ip(config.ipv4.mask, netmask);
    return true;
}

bool set_ap_config(
    void *,
    const uint8_t ip[4],
    const uint8_t gateway[4],
    const uint8_t netmask[4])
{
    if (ip == nullptr || gateway == nullptr || netmask == nullptr) {
        return false;
    }
    net_al_ext_ip_addr_cfg config = {};
    config.mode = IP_ADDR_STATIC_IPV4;
    config.default_output = false;
    config.ipv4.addr = make_ip(ip);
    config.ipv4.gw = make_ip(gateway);
    config.ipv4.mask = make_ip(netmask);
    config.ipv4.dns = 0U;
    return net_al_ext_set_vif_ip(MGMR_VIF_AP, &config) == 0;
}

} // namespace

bool bouffalo_wifi_ap_ready()
{
    LOCK_TCPIP_CORE();
    const auto *ap = static_cast<const netif *>(fhost_to_net_if(MGMR_VIF_AP));
    const bool ready = ap != nullptr && netif_is_up(ap);
    UNLOCK_TCPIP_CORE();
    return ready;
}

const WifiBackend &bouffalo_wifi_backend()
{
    static const WifiBackend backend = {
        nullptr,
        start_scan,
        copy_scan_results,
        start_sta_connect,
        set_sta_dns,
        disconnect_sta,
        set_auto_reconnect,
        set_sta_role_enabled,
        enable_sta_coexistence,
        get_sta_ssid,
        get_sta_rssi,
        get_sta_bssid,
        get_sta_ip_info,
        start_softap,
        stop_softap,
        get_ap_mac,
        get_ap_ip,
        set_ap_config,
        set_sta_hostname,
        get_sta_hostname,
        enable_sta_ipv6,
        get_sta_ipv6,
        get_sta_mac,
    };
    return backend;
}

} // namespace bridge
