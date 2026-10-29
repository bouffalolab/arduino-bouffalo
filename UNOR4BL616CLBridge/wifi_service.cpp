#include "wifi_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace bridge {

namespace {

constexpr char kWifiScanCommand[] = "+WIFISCAN";
constexpr char kWifiModeCommand[] = "+WIFIMODE";
constexpr char kBeginStaCommand[] = "+BEGINSTA";
constexpr char kSetIpCommand[] = "+SETIP";
constexpr char kSoftResetWifiCommand[] = "+SOFTRESETWIFI";
constexpr char kReconnectCommand[] = "+RECONNECT";
constexpr char kGetRssiCommand[] = "+GETRSSI";
constexpr char kIpStaCommand[] = "+IPSTA";
constexpr char kGetSsidCommand[] = "+GETSSID";
constexpr char kGetBssidCommand[] = "+GETBSSID";
constexpr char kMacStaCommand[] = "+MACSTA";
constexpr char kAutoConnectCommand[] = "+AUTOCONNECT";
constexpr char kDisconnectCommand[] = "+DISCONNECT";
constexpr char kBeginSoftApCommand[] = "+BEGINSOFTAP";
constexpr char kDisconnectSoftApCommand[] = "+DISCONNECTSOFTAP";
constexpr char kMacSoftApCommand[] = "+MACSOFTAP";
constexpr char kIpSoftApCommand[] = "+IPSOFTAP";
constexpr char kGetSoftApSsidCommand[] = "+GETSOFTAPSSID";
constexpr char kSoftApConfigCommand[] = "+SOFTAPCONFIG";
constexpr char kHostnameCommand[] = "+HOSTNAME";
constexpr char kIpv6Command[] = "+IPV6";

bool is_run_command(const at::Command &command, const char *name)
{
    return command.mode == at::CommandMode::Run && strcmp(command.name, name) == 0;
}

bool command_is(const at::Command &command, const char *name)
{
    return strcmp(command.name, name) == 0;
}

} // namespace

WifiService::WifiService(
    WifiStatusTracker &status_tracker,
    WifiBackend backend,
    WifiWaitHooks wait_hooks)
    : status_tracker_(status_tracker),
      backend_(backend),
      wait_hooks_(wait_hooks)
{
}

bool WifiService::handles_command(const at::Command &command) const
{
    return command_is(command, kWifiScanCommand) ||
        command_is(command, kWifiModeCommand) ||
        command_is(command, kBeginStaCommand) ||
        command_is(command, kSetIpCommand) ||
        command_is(command, kSoftResetWifiCommand) ||
        command_is(command, kReconnectCommand) ||
        command_is(command, kGetRssiCommand) ||
        command_is(command, kIpStaCommand) ||
        command_is(command, kGetSsidCommand) ||
        command_is(command, kGetBssidCommand) ||
        command_is(command, kMacStaCommand) ||
        command_is(command, kAutoConnectCommand) ||
        command_is(command, kDisconnectCommand) ||
        command_is(command, kBeginSoftApCommand) ||
        command_is(command, kDisconnectSoftApCommand) ||
        command_is(command, kMacSoftApCommand) ||
        command_is(command, kIpSoftApCommand) ||
        command_is(command, kGetSoftApSsidCommand) ||
        command_is(command, kSoftApConfigCommand) ||
        command_is(command, kHostnameCommand) ||
        command_is(command, kIpv6Command);
}

at::CommandResult WifiService::handle_command(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command_is(command, kWifiScanCommand)) {
        if (!is_run_command(command, kWifiScanCommand)) {
            return at::CommandResult::Error;
        }
        if (!start_scan() || !wait_for_scan()) {
            return at::CommandResult::Error;
        }
        return write_scan_response(transport) ? at::CommandResult::Ok : at::CommandResult::Error;
    }

    if (command_is(command, kWifiModeCommand)) {
        return handle_wifi_mode(command, transport);
    }

    if (command_is(command, kBeginStaCommand)) {
        return handle_begin_sta(command, transport);
    }

    if (command_is(command, kSetIpCommand)) {
        return handle_set_ip(command, transport);
    }

    if (command_is(command, kSoftResetWifiCommand)) {
        return handle_soft_reset_wifi(command, transport);
    }

    if (command_is(command, kReconnectCommand)) {
        return handle_reconnect(command, transport);
    }

    if (command_is(command, kGetRssiCommand)) {
        return handle_sta_rssi(command, transport);
    }

    if (command_is(command, kIpStaCommand)) {
        return handle_sta_ip(command, transport);
    }

    if (command_is(command, kGetSsidCommand)) {
        return handle_sta_ssid(command, transport);
    }

    if (command_is(command, kGetBssidCommand)) {
        return handle_sta_bssid(command, transport);
    }

    if (command_is(command, kMacStaCommand)) {
        return handle_sta_mac(command, transport);
    }

    if (command_is(command, kAutoConnectCommand)) {
        return handle_auto_connect(command, transport);
    }

    if (command_is(command, kDisconnectCommand)) {
        return handle_disconnect(command, transport);
    }

    if (command_is(command, kBeginSoftApCommand)) {
        return handle_begin_softap(command, transport);
    }

    if (command_is(command, kDisconnectSoftApCommand)) {
        return handle_disconnect_softap(command, transport);
    }

    if (command_is(command, kMacSoftApCommand)) {
        return handle_mac_softap(command, transport);
    }

    if (command_is(command, kIpSoftApCommand)) {
        return handle_ip_softap(command, transport);
    }

    if (command_is(command, kGetSoftApSsidCommand)) {
        return handle_softap_ssid(command, transport);
    }

    if (command_is(command, kSoftApConfigCommand)) {
        return handle_softap_config(command, transport);
    }

    if (command_is(command, kHostnameCommand)) {
        return handle_hostname(command, transport);
    }

    if (command_is(command, kIpv6Command)) {
        return handle_ipv6(command, transport);
    }

    return at::CommandResult::Error;
}

at::CommandResult WifiService::handle_hostname(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode == at::CommandMode::Read) {
        if (backend_.get_sta_hostname == nullptr) {
            return at::CommandResult::Error;
        }
        uint8_t hostname[kWifiHostnameBytes + 1U] = {};
        size_t hostname_size = 0U;
        if (!backend_.get_sta_hostname(
                backend_.context,
                hostname,
                kWifiHostnameBytes,
                &hostname_size) ||
            hostname_size == 0U ||
            hostname_size > kWifiHostnameBytes) {
            return at::CommandResult::Error;
        }
        hostname[hostname_size] = '\0';
        return transport.write_response(
                    command,
                    reinterpret_cast<const char *>(hostname))
            ? at::CommandResult::Ok
            : at::CommandResult::Error;
    }

    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        command.arguments[0] == nullptr ||
        command.arguments[0][0] == '\0' ||
        strlen(command.arguments[0]) > kWifiHostnameBytes ||
        backend_.set_sta_hostname == nullptr) {
        return at::CommandResult::Error;
    }

    const size_t hostname_size = strlen(command.arguments[0]);
    if (!backend_.set_sta_hostname(
            backend_.context,
            reinterpret_cast<const uint8_t *>(command.arguments[0]),
            hostname_size)) {
        return at::CommandResult::Error;
    }
    return transport.write_response(command, "")
        ? at::CommandResult::Ok
        : at::CommandResult::Error;
}

at::CommandResult WifiService::handle_ipv6(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode == at::CommandMode::Run) {
        return backend_.enable_sta_ipv6 != nullptr &&
                backend_.enable_sta_ipv6(backend_.context)
            ? at::CommandResult::Ok
            : at::CommandResult::Error;
    }

    if (command.mode != at::CommandMode::Read ||
        backend_.get_sta_ipv6 == nullptr) {
        return at::CommandResult::Error;
    }

    char address[kWifiIpv6TextBytes + 1U] = {};
    if (!backend_.get_sta_ipv6(
            backend_.context,
            address,
            sizeof(address)) ||
        address[0] == '\0') {
        return at::CommandResult::Error;
    }
    return transport.write_response(command, address)
        ? at::CommandResult::Ok
        : at::CommandResult::Error;
}

bool WifiService::start_scan()
{
    if (wifi_mode_ == 0U || scan_active_ || backend_.start_scan == nullptr) {
        return false;
    }

    scan_result_count_ = 0U;
    scan_complete_ = false;
    scan_active_ = true;
    if (backend_.start_scan(backend_.context)) {
        return true;
    }

    scan_active_ = false;
    return false;
}

bool WifiService::scan_active() const
{
    return scan_active_;
}

bool WifiService::scan_complete() const
{
    return scan_complete_;
}

size_t WifiService::scan_result_count()
{
    if (!scan_complete_ || backend_.copy_scan_results == nullptr) {
        return 0U;
    }

    scan_result_count_ = backend_.copy_scan_results(
        backend_.context,
        scan_results_,
        sizeof(scan_results_) / sizeof(scan_results_[0]));
    if (scan_result_count_ > kWifiScanMaxResults) {
        scan_result_count_ = kWifiScanMaxResults;
    }
    return scan_result_count_;
}

const WifiScanResult *WifiService::scan_results() const
{
    return scan_results_;
}

bool WifiService::poll()
{
    if (sta_dns_restore_pending_) {
        sta_dns_restore_pending_ = false;
        if (wifi_mode_ != 0U && !sta_network_config_.configured &&
            sta_network_config_.dns_configured) {
            // Native DNS takes the lwIP mutex. SDK Wi-Fi callbacks run in
            // the timer daemon with scheduling suspended, so do this only
            // from the bridge task before publishing IPv4 readiness.
            if (backend_.set_sta_dns != nullptr) {
                (void)backend_.set_sta_dns(
                    backend_.context,
                    sta_network_config_.dns1,
                    sta_network_config_.dns2);
            }
            status_tracker_.on_event(WifiStatusEvent::StaConnected);
            if (!sta_coexistence_active_) {
                sta_coexistence_pending_ = true;
            }
        }
        return true;
    }
    if (sta_coexistence_pending_) {
        sta_coexistence_pending_ = false;
        sta_coexistence_active_ =
            backend_.enable_sta_coexistence != nullptr &&
            backend_.enable_sta_coexistence(backend_.context);
        return true;
    }

    if (ap_start_pending_ &&
        (wait_hooks_.now_ms == nullptr ||
         static_cast<int32_t>(
             wait_hooks_.now_ms(wait_hooks_.context) - ap_start_not_before_ms_) >= 0)) {
        /*
         * BEGIN_SOFTAP must first finish its AT response. Starting the native
         * backend from the command-dispatch stack can block that response before
         * the request reaches the RA4M1 UART. Consume one queued request only
         * after Ra4AtService observed an output-idle poll cycle. Keep a physical
         * UART grace interval as well: a queued response is not necessarily on
         * the wire when a native FHOST stall can stop UART servicing.
         */
        ap_start_pending_ = false;
        const bool accepted = backend_.start_softap != nullptr &&
            backend_.start_softap(
                backend_.context,
                pending_ap_ssid_,
                pending_ap_ssid_size_,
                pending_ap_password_size_ == 0U ? nullptr : pending_ap_password_,
                pending_ap_password_size_,
                pending_ap_channel_,
                pending_ap_hidden_,
                pending_ap_max_connections_,
                &pending_ap_network_config_);
        if (!accepted) {
            status_tracker_.set_status(WifiStatus::ApFailed);
        }
        return true;
    }

    return false;
}

bool WifiService::disconnect_for_bootstrap()
{
    if (!sta_connection_active()) {
        return true;
    }
    if (backend_.disconnect_sta == nullptr ||
        !backend_.disconnect_sta(backend_.context, false)) {
        return false;
    }
    sta_connect_pending_ = false;
    sta_dns_restore_pending_ = false;
    sta_coexistence_pending_ = false;
    sta_coexistence_active_ = false;
    status_tracker_.set_status(WifiStatus::Idle);
    return true;
}

void WifiService::on_wifi_event(uint16_t code)
{
    switch (static_cast<WifiEventCode>(code)) {
    case WifiEventCode::InitDone:
        if (wifi_mode_ != 0U) {
            status_tracker_.on_event(WifiStatusEvent::InterfaceReady);
        }
        break;
    case WifiEventCode::ScanDone:
        if (wifi_mode_ != 0U) {
            status_tracker_.on_event(WifiStatusEvent::ScanDone);
        }
        if (scan_active_) {
            scan_complete_ = true;
            scan_active_ = false;
        }
        break;
    case WifiEventCode::Connected:
        /*
         * CODE_WIFI_ON_CONNECTED only confirms 802.11 association. WiFiS3
         * returns from WiFi.begin() when GETSTATUS reports WL_CONNECTED, so
         * publishing that state here can start sockets before DHCP assigns an
         * IPv4 address. DHCP connections therefore wait for
         * CODE_WIFI_ON_GOT_IP instead.
         *
         * The Bouffalo fhost SDK intentionally skips DHCP when
         * wifi_mgmr_sta_connect_params_t::use_dhcp is zero. The static
         * address is installed by +SETIP before association, so this
         * association event is the usable IPv4-ready transition for that
         * mode.
         */
        if (sta_network_config_.configured && wifi_mode_ != 0U) {
            sta_connect_pending_ = false;
            status_tracker_.on_event(WifiStatusEvent::StaConnected);
            if (!sta_coexistence_active_) {
                sta_coexistence_pending_ = true;
            }
        }
        break;
    case WifiEventCode::GotIp:
        sta_connect_pending_ = false;
        /*
         * DHCP installs the DNS servers supplied by the lease. Reapply a
         * WiFiS3::setDNS() override only after that lifecycle point so it
         * survives reconnects without converting the STA to static IPv4.
         */
        if (!sta_network_config_.configured &&
            sta_network_config_.dns_configured &&
            backend_.set_sta_dns != nullptr) {
            sta_dns_restore_pending_ = true;
            break;
        }
        if (wifi_mode_ != 0U) {
            status_tracker_.on_event(WifiStatusEvent::StaConnected);
            if (!sta_coexistence_active_) {
                sta_coexistence_pending_ = true;
            }
        }
        break;
    case WifiEventCode::Disconnected:
        sta_connect_pending_ = false;
        sta_dns_restore_pending_ = false;
        sta_coexistence_pending_ = false;
        sta_coexistence_active_ = false;
        if (wifi_mode_ != 0U) {
            status_tracker_.on_event(WifiStatusEvent::StaDisconnected);
        }
        break;
    case WifiEventCode::ApStarted:
        ap_started_ = true;
        status_tracker_.on_event(WifiStatusEvent::ApStarted);
        break;
    case WifiEventCode::ApStopped:
        ap_started_ = false;
        ap_ssid_size_ = 0U;
        status_tracker_.on_event(WifiStatusEvent::ApStopped);
        break;
    case WifiEventCode::ApStationAdded:
        status_tracker_.on_event(WifiStatusEvent::ApStaConnected);
        break;
    case WifiEventCode::ApStationRemoved:
        status_tracker_.on_event(WifiStatusEvent::ApStaDisconnected);
        break;
    case WifiEventCode::ManagerDone:
        break;
    }
}

at::CommandResult WifiService::handle_wifi_mode(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode == at::CommandMode::Read) {
        char response[4] = {};
        const int response_size = snprintf(
            response,
            sizeof(response),
            "%u\r\n",
            static_cast<unsigned int>(wifi_mode_));
        return response_size > 0 &&
                static_cast<size_t>(response_size) < sizeof(response) &&
                transport.write_response(command, response)
            ? at::CommandResult::Ok
            : at::CommandResult::Error;
    }

    if (command.mode != at::CommandMode::Write ||
        (command.argument_count != 1U && command.argument_count != 2U) ||
        command.arguments[0][0] == '\0') {
        return at::CommandResult::Error;
    }

    /*
     * The ESP32-S3 command accepts the optional deprecated auto-connect
     * argument. Keep that parsing surface while mapping all four role values.
     */
    const int requested_mode = atoi(command.arguments[0]);
    if (requested_mode < 0 || requested_mode > 3) {
        return at::CommandResult::Error;
    }

    const bool wants_sta = requested_mode == 1 || requested_mode == 3;
    const bool wants_ap = requested_mode == 2 || requested_mode == 3;

    if (backend_.set_sta_role_enabled == nullptr ||
        !backend_.set_sta_role_enabled(backend_.context, wants_sta)) {
        return at::CommandResult::Error;
    }

    if (!wants_ap && ap_started_) {
        if (backend_.stop_softap == nullptr ||
            !backend_.stop_softap(backend_.context, false)) {
            return at::CommandResult::Error;
        }
        ap_started_ = false;
        ap_ssid_size_ = 0U;
        status_tracker_.on_event(WifiStatusEvent::ApStopped);
    }

    wifi_mode_ = static_cast<uint8_t>(requested_mode);
    scan_active_ = false;
    scan_complete_ = false;
    scan_result_count_ = 0U;

    if (wifi_mode_ == 0U) {
        sta_dns_restore_pending_ = false;
        sta_connect_pending_ = false;
        status_tracker_.set_status(WifiStatus::NoModule);
    } else {
        /*
         * WIFIMODE changes the AT-visible role only. Do not issue the native
         * reconnect command here: on BL616CL the STA VIF may not exist until
         * BEGINSTA creates it, and fhost's reconnect command is not safe
         * before that lifecycle point. BEGINSTA and RECONNECT apply the
         * stored policy once a STA context is active.
         */
        if (!ap_started_) {
            status_tracker_.set_status(WifiStatus::Idle);
        }
    }

    return transport.write_response(command, "")
        ? at::CommandResult::Ok
        : at::CommandResult::Error;
}

at::CommandResult WifiService::handle_begin_sta(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (!sta_mode_enabled() ||
        command.mode != at::CommandMode::Write ||
        (command.argument_count != 1U && command.argument_count != 2U)) {
        return at::CommandResult::Error;
    }

    const char *ssid = command.arguments[0];
    const size_t ssid_size = strlen(ssid);
    if (ssid_size == 0U || ssid_size > kWifiStaSsidBytes) {
        return at::CommandResult::Error;
    }

    const char *password = nullptr;
    size_t password_size = 0U;
    if (command.argument_count == 2U) {
        password = command.arguments[1];
        password_size = strlen(password);
        if (password_size == 0U || password_size > kWifiStaPasswordBytes) {
            return at::CommandResult::Error;
        }
    }

    /*
     * ESP32 WiFi.begin() returns an immediate wl_status_t snapshot and its
     * handler still appends OK when the start attempt fails. Mirror that
     * behavior without claiming that the asynchronous association completed.
     */
    const bool accepted = backend_.start_sta_connect != nullptr &&
        backend_.start_sta_connect(
            backend_.context,
            reinterpret_cast<const uint8_t *>(ssid),
            ssid_size,
            reinterpret_cast<const uint8_t *>(password),
            password_size,
            &sta_network_config_,
            auto_reconnect_enabled_);
    const WifiStatus immediate_status =
        accepted ? status_tracker_.status() : WifiStatus::ConnectFailed;
    sta_connect_pending_ = accepted;
    return write_status_response(command, transport, immediate_status)
        ? at::CommandResult::Ok
        : at::CommandResult::Error;
}

at::CommandResult WifiService::handle_set_ip(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count < 3U ||
        command.argument_count > 5U) {
        return at::CommandResult::Error;
    }

    WifiStaNetworkConfig config;
    if (!parse_ipv4(command.arguments[0], config.ip) ||
        !parse_ipv4(command.arguments[1], config.gateway) ||
        !parse_ipv4(command.arguments[2], config.netmask)) {
        return at::CommandResult::Error;
    }
    if (command.argument_count >= 4U &&
        !parse_ipv4(command.arguments[3], config.dns1)) {
        return at::CommandResult::Error;
    }
    if (command.argument_count >= 5U &&
        !parse_ipv4(command.arguments[4], config.dns2)) {
        return at::CommandResult::Error;
    }

    config.dns_configured = command.argument_count >= 4U;
    WifiStaIpInfo current = {};
    const bool preserves_dhcp_lease =
        !sta_network_config_.configured &&
        config.dns_configured &&
        status_tracker_.status() == WifiStatus::Connected &&
        backend_.get_sta_ip_info != nullptr &&
        backend_.get_sta_ip_info(backend_.context, &current) &&
        memcmp(config.ip, current.ip, sizeof(config.ip)) == 0 &&
        memcmp(config.gateway, current.gateway, sizeof(config.gateway)) == 0 &&
        memcmp(config.netmask, current.netmask, sizeof(config.netmask)) == 0;
    if (preserves_dhcp_lease) {
        if (backend_.set_sta_dns == nullptr ||
            !backend_.set_sta_dns(
                backend_.context,
                config.dns1,
                config.dns2)) {
            return at::CommandResult::Error;
        }
        /*
         * Retain DHCP addressing, but remember the requested resolver pair
         * for the next CODE_WIFI_ON_GOT_IP event after a reconnect.
         */
        sta_network_config_ = {};
        sta_network_config_.dns_configured = true;
        memcpy(sta_network_config_.dns1, config.dns1, sizeof(config.dns1));
        memcpy(sta_network_config_.dns2, config.dns2, sizeof(config.dns2));
        return transport.write_response(command, "")
            ? at::CommandResult::Ok
            : at::CommandResult::Error;
    }

    config.configured = true;
    sta_network_config_ = config;
    return transport.write_response(command, "")
        ? at::CommandResult::Ok
        : at::CommandResult::Error;
}

at::CommandResult WifiService::handle_begin_softap(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (!ap_mode_enabled() ||
        command.mode != at::CommandMode::Write ||
        command.argument_count < 1U ||
        command.argument_count > 5U) {
        return at::CommandResult::Error;
    }

    const char *ssid = command.arguments[0];
    const size_t ssid_size = strlen(ssid);
    if (ssid_size == 0U || ssid_size > kWifiApSsidBytes) {
        return at::CommandResult::Error;
    }

    const char *password = nullptr;
    size_t password_size = 0U;
    if (command.argument_count >= 2U) {
        password = command.arguments[1];
        password_size = strlen(password);
        if (password_size != 0U &&
            (password_size < 8U || password_size > 63U)) {
            return at::CommandResult::Error;
        }
    }

    uint8_t channel = 1U;
    bool hidden = false;
    uint8_t max_connections = 4U;
    if (command.argument_count >= 3U &&
        !parse_uint8(command.arguments[2], 1U, 13U, channel)) {
        return at::CommandResult::Error;
    }
    if (command.argument_count >= 4U &&
        !parse_esp32_bool(command.arguments[3], hidden)) {
        return at::CommandResult::Error;
    }
    /*
     * The current bridge bundle is built with CFG_STA_MAX=4. Refuse a larger
     * request rather than silently advertising a connection limit the radio
     * cannot provide.
     */
    if (command.argument_count >= 5U &&
        !parse_uint8(command.arguments[4], 1U, 4U, max_connections)) {
        return at::CommandResult::Error;
    }

    if (ap_start_pending_) {
        return at::CommandResult::Error;
    }

    memcpy(pending_ap_ssid_, ssid, ssid_size);
    pending_ap_ssid_size_ = ssid_size;
    if (password_size != 0U) {
        memcpy(pending_ap_password_, password, password_size);
    }
    pending_ap_password_size_ = password_size;
    pending_ap_channel_ = channel;
    pending_ap_hidden_ = hidden;
    pending_ap_max_connections_ = max_connections;
    pending_ap_network_config_ = ap_network_config_;
    memcpy(ap_ssid_, ssid, ssid_size);
    ap_ssid_size_ = ssid_size;
    ap_start_pending_ = true;
    ap_start_not_before_ms_ = wait_hooks_.now_ms != nullptr
        ? wait_hooks_.now_ms(wait_hooks_.context) + kSoftApResponseGraceMs
        : 0U;

    return transport.write_response(command, "1")
        ? at::CommandResult::Ok
        : at::CommandResult::Error;
}

at::CommandResult WifiService::handle_disconnect_softap(
    const at::Command &command,
    at::AtTransport &transport)
{
    bool wifi_off = false;
    if (command.mode == at::CommandMode::Write) {
        if (command.argument_count != 1U ||
            !parse_esp32_bool(command.arguments[0], wifi_off)) {
            return at::CommandResult::Error;
        }
    } else if (command.mode != at::CommandMode::Run) {
        return at::CommandResult::Error;
    }

    if (ap_start_pending_) {
        ap_start_pending_ = false;
        ap_start_not_before_ms_ = 0U;
        pending_ap_ssid_size_ = 0U;
        pending_ap_password_size_ = 0U;
        ap_ssid_size_ = 0U;
        status_tracker_.on_event(WifiStatusEvent::ApStopped);
        (void)transport;
        return at::CommandResult::Ok;
    }

    const bool stopped = backend_.stop_softap != nullptr &&
        backend_.stop_softap(backend_.context, wifi_off);
    if (!stopped && wifi_off) {
        return at::CommandResult::Error;
    }
    ap_started_ = false;
    ap_ssid_size_ = 0U;
    status_tracker_.on_event(WifiStatusEvent::ApStopped);
    (void)transport;
    return at::CommandResult::Ok;
}

at::CommandResult WifiService::handle_mac_softap(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Read ||
        backend_.get_ap_mac == nullptr) {
        return at::CommandResult::Error;
    }

    uint8_t mac[6] = {};
    if (!backend_.get_ap_mac(backend_.context, mac)) {
        return at::CommandResult::Error;
    }
    char text[18] = {};
    const int size = snprintf(
        text,
        sizeof(text),
        "%02x:%02x:%02x:%02x:%02x:%02x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return size == 17 && transport.write_response(command, text)
        ? at::CommandResult::Ok
        : at::CommandResult::Error;
}

at::CommandResult WifiService::handle_ip_softap(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Run ||
        backend_.get_ap_ip == nullptr) {
        return at::CommandResult::Error;
    }

    uint8_t ip[4] = {};
    uint8_t gateway[4] = {};
    uint8_t netmask[4] = {};
    if (!backend_.get_ap_ip(backend_.context, ip, gateway, netmask) ||
        !transport.write_cstr(command.name) ||
        !transport.write_cstr(": ") ||
        !write_ipv4(transport, ip) ||
        !transport.write_cstr("\r\n")) {
        return at::CommandResult::Error;
    }
    return at::CommandResult::Ok;
}

at::CommandResult WifiService::handle_softap_ssid(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Read ||
        ap_ssid_size_ == 0U ||
        !transport.write_cstr(command.name) ||
        !transport.write_cstr(": ") ||
        !transport.write_data(ap_ssid_, ap_ssid_size_) ||
        !transport.write_cstr("\r\n")) {
        return at::CommandResult::Error;
    }
    return at::CommandResult::Ok;
}

at::CommandResult WifiService::handle_softap_config(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 3U) {
        return at::CommandResult::Error;
    }

    WifiApNetworkConfig config;
    config.configured = true;
    if (!parse_ipv4(command.arguments[0], config.ip) ||
        !parse_ipv4(command.arguments[1], config.gateway) ||
        !parse_ipv4(command.arguments[2], config.netmask)) {
        return at::CommandResult::Error;
    }

    /*
     * Bouffalo consumes this configuration at AP start. When an AP is already
     * running, also apply it to the live AP VIF so SOFTAPCONFIG retains the
     * ESP32 API's immediate effect.
     */
    if (ap_started_ &&
        (backend_.set_ap_config == nullptr ||
         !backend_.set_ap_config(
             backend_.context,
             config.ip,
             config.gateway,
             config.netmask))) {
        return at::CommandResult::Error;
    }
    ap_network_config_ = config;
    return transport.write_response(command, "")
        ? at::CommandResult::Ok
        : at::CommandResult::Error;
}

at::CommandResult WifiService::handle_soft_reset_wifi(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (!is_run_command(command, kSoftResetWifiCommand)) {
        return at::CommandResult::Error;
    }

    /*
     * The ESP32-S3 handler resets every network resource, clears static IP
     * configuration, then ignores disconnect return values. Socket resources
     * are not implemented on the provisional BL616CL bridge yet, but WiFiS3
     * requires this command to acknowledge its modem bootstrap.
     */
    if (sta_connection_active() && backend_.disconnect_sta != nullptr) {
        (void)backend_.disconnect_sta(backend_.context, false);
    }
    sta_connect_pending_ = false;
    sta_dns_restore_pending_ = false;
    sta_network_config_ = {};
    ap_start_pending_ = false;
    ap_start_not_before_ms_ = 0U;
    pending_ap_ssid_size_ = 0U;
    pending_ap_password_size_ = 0U;
    if (ap_started_ && backend_.stop_softap != nullptr) {
        (void)backend_.stop_softap(backend_.context, false);
        ap_started_ = false;
        ap_ssid_size_ = 0U;
    }
    scan_active_ = false;
    scan_complete_ = false;
    scan_result_count_ = 0U;
    status_tracker_.set_status(WifiStatus::Idle);
    return transport.write_response(command, "")
        ? at::CommandResult::Ok
        : at::CommandResult::Error;
}

at::CommandResult WifiService::handle_reconnect(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode == at::CommandMode::Read) {
        return transport.write_response(
                   command,
                   auto_reconnect_enabled_ ? "1" : "0")
            ? at::CommandResult::Ok
            : at::CommandResult::Error;
    }

    bool enabled = false;
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        !parse_esp32_bool(command.arguments[0], enabled)) {
        return at::CommandResult::Error;
    }

    /*
     * ESP32-S3 stores this setting locally and ignores the setter result in
     * the AT handler. Keep that observable behavior while still asking the
     * native backend to apply the corresponding reconnect policy.
     */
    if (backend_.set_auto_reconnect != nullptr) {
        (void)backend_.set_auto_reconnect(backend_.context, enabled);
    }
    auto_reconnect_enabled_ = enabled;
    return at::CommandResult::Ok;
}

at::CommandResult WifiService::handle_sta_rssi(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Read ||
        backend_.get_sta_rssi == nullptr) {
        return at::CommandResult::Error;
    }

    int rssi = 0;
    if (!backend_.get_sta_rssi(backend_.context, &rssi)) {
        return at::CommandResult::Error;
    }
    char text[16] = {};
    const int size = snprintf(text, sizeof(text), "%d", rssi);
    return size > 0 &&
            static_cast<size_t>(size) < sizeof(text) &&
            transport.write_response(command, text)
        ? at::CommandResult::Ok
        : at::CommandResult::Error;
}

at::CommandResult WifiService::handle_sta_ip(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        command.arguments[0][0] == '\0' ||
        backend_.get_sta_ip_info == nullptr) {
        return at::CommandResult::Error;
    }

    /*
     * Keep the ESP32-S3 handler's atoi-compatible selector behavior:
     * 0=IPv4, 1=gateway, 2=netmask, 3=DNS1, 4=DNS2.
     */
    const int requested = atoi(command.arguments[0]);
    if (requested < 0 || requested > 4) {
        return at::CommandResult::Error;
    }

    WifiStaIpInfo info = {};
    if (!backend_.get_sta_ip_info(backend_.context, &info)) {
        return at::CommandResult::Error;
    }

    const uint8_t *address = info.ip;
    switch (requested) {
    case 1:
        address = info.gateway;
        break;
    case 2:
        address = info.netmask;
        break;
    case 3:
        address = info.dns1;
        break;
    case 4:
        address = info.dns2;
        break;
    default:
        break;
    }

    char text[16] = {};
    const int size = snprintf(
        text,
        sizeof(text),
        "%u.%u.%u.%u",
        static_cast<unsigned int>(address[0]),
        static_cast<unsigned int>(address[1]),
        static_cast<unsigned int>(address[2]),
        static_cast<unsigned int>(address[3]));
    return size > 0 &&
            static_cast<size_t>(size) < sizeof(text) &&
            transport.write_response(command, text)
        ? at::CommandResult::Ok
        : at::CommandResult::Error;
}

at::CommandResult WifiService::handle_sta_ssid(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Read ||
        backend_.get_sta_ssid == nullptr) {
        return at::CommandResult::Error;
    }

    uint8_t ssid[kWifiStaSsidBytes] = {};
    size_t ssid_size = 0U;
    if (!backend_.get_sta_ssid(
            backend_.context,
            ssid,
            sizeof(ssid),
            &ssid_size) ||
        ssid_size == 0U ||
        ssid_size > sizeof(ssid) ||
        !transport.write_cstr(command.name) ||
        !transport.write_cstr(": ") ||
        !transport.write_data(ssid, ssid_size) ||
        !transport.write_cstr("\r\n")) {
        return at::CommandResult::Error;
    }
    return at::CommandResult::Ok;
}

at::CommandResult WifiService::handle_sta_bssid(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Read ||
        backend_.get_sta_bssid == nullptr) {
        return at::CommandResult::Error;
    }

    uint8_t bssid[6] = {};
    if (!backend_.get_sta_bssid(backend_.context, bssid)) {
        return at::CommandResult::Error;
    }
    char text[18] = {};
    const int size = snprintf(
        text,
        sizeof(text),
        "%02x:%02x:%02x:%02x:%02x:%02x",
        bssid[0],
        bssid[1],
        bssid[2],
        bssid[3],
        bssid[4],
        bssid[5]);
    return size == 17 && transport.write_response(command, text)
        ? at::CommandResult::Ok
        : at::CommandResult::Error;
}

at::CommandResult WifiService::handle_sta_mac(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Read ||
        backend_.get_sta_mac == nullptr) {
        return at::CommandResult::Error;
    }
    uint8_t mac[6] = {};
    if (!backend_.get_sta_mac(backend_.context, mac)) {
        return at::CommandResult::Error;
    }
    char text[18] = {};
    const int size = snprintf(
        text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return size == 17 && transport.write_response(command, text)
        ? at::CommandResult::Ok
        : at::CommandResult::Error;
}

at::CommandResult WifiService::handle_auto_connect(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode == at::CommandMode::Read) {
        return transport.write_response(command, "0")
            ? at::CommandResult::Ok
            : at::CommandResult::Error;
    }

    /*
     * ESP32-S3 0.6.0 keeps this deprecated API as a no-op: its AT handler
     * accepts one nonempty argument, ignores setAutoConnect() returning
     * false, and preserves a readback of zero. Do not map this to Bouffalo's
     * native auto-connect feature; RECONNECT owns that observable behavior.
     */
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        command.arguments[0][0] == '\0') {
        return at::CommandResult::Error;
    }
    return at::CommandResult::Ok;
}

at::CommandResult WifiService::handle_disconnect(
    const at::Command &command,
    at::AtTransport &transport)
{
    bool wifi_off = false;
    if (command.mode == at::CommandMode::Write) {
        if (command.argument_count != 1U ||
            !parse_esp32_bool(command.arguments[0], wifi_off)) {
            return at::CommandResult::Error;
        }
    } else if (command.mode != at::CommandMode::Run) {
        return at::CommandResult::Error;
    }

    if (backend_.disconnect_sta == nullptr ||
        !backend_.disconnect_sta(backend_.context, wifi_off)) {
        /*
         * For normal disconnect, preserve the ESP32-S3 handler's OK framing:
         * it ignores WiFi.disconnect()'s boolean result. A backend rejects
         * wifi_off only when it cannot actually turn the Wi-Fi radio off.
         */
        if (wifi_off) {
            return at::CommandResult::Error;
        }
    }
    sta_connect_pending_ = false;
    sta_dns_restore_pending_ = false;

    if (command.mode == at::CommandMode::Run &&
        !transport.write_response(command, "")) {
        return at::CommandResult::Error;
    }
    return at::CommandResult::Ok;
}

bool WifiService::wait_for_scan()
{
    if (wait_hooks_.now_ms == nullptr || wait_hooks_.idle == nullptr) {
        return false;
    }

    const uint32_t started_at = wait_hooks_.now_ms(wait_hooks_.context);
    while (!scan_complete_) {
        if (static_cast<uint32_t>(
                wait_hooks_.now_ms(wait_hooks_.context) - started_at) >=
            kScanTimeoutMs) {
            scan_active_ = false;
            return false;
        }
        wait_hooks_.idle(wait_hooks_.context);
    }
    return true;
}

bool WifiService::write_scan_response(at::AtTransport &transport)
{
    const size_t result_count = scan_result_count();
    if (result_count == 0U) {
        return true;
    }

    size_t payload_size = 0U;
    if (!scan_payload_size(result_count, payload_size)) {
        return false;
    }

    char header[32] = {};
    const int header_size = snprintf(
        header,
        sizeof(header),
        "+WIFISCAN: %lu|",
        static_cast<unsigned long>(payload_size));
    if (header_size < 0 ||
        static_cast<size_t>(header_size) >= sizeof(header) ||
        !transport.write_cstr(header)) {
        return false;
    }

    for (size_t index = 0U; index < result_count; ++index) {
        const WifiScanResult &result = scan_results_[index];
        char bssid[18] = {};
        char values[32] = {};
        const int bssid_size = snprintf(
            bssid,
            sizeof(bssid),
            "%02x:%02x:%02x:%02x:%02x:%02x",
            result.bssid[0],
            result.bssid[1],
            result.bssid[2],
            result.bssid[3],
            result.bssid[4],
            result.bssid[5]);
        const int values_size = snprintf(
            values,
            sizeof(values),
            " | %d | %u | %s\r\n",
            static_cast<int>(result.rssi),
            static_cast<unsigned int>(result.channel),
            auth_text(result.auth));
        if (bssid_size < 0 || values_size < 0 ||
            static_cast<size_t>(bssid_size) >= sizeof(bssid) ||
            static_cast<size_t>(values_size) >= sizeof(values) ||
            result.ssid_size > sizeof(result.ssid) ||
            !transport.write_data(result.ssid, result.ssid_size) ||
            !transport.write_cstr(" | ") ||
            !transport.write_cstr(bssid) ||
            !transport.write_cstr(values)) {
            return false;
        }
    }
    return true;
}

bool WifiService::scan_payload_size(size_t result_count, size_t &payload_size) const
{
    payload_size = 0U;
    for (size_t index = 0U; index < result_count; ++index) {
        const WifiScanResult &result = scan_results_[index];
        char bssid[18] = {};
        char values[32] = {};
        const int bssid_size = snprintf(
            bssid,
            sizeof(bssid),
            "%02x:%02x:%02x:%02x:%02x:%02x",
            result.bssid[0],
            result.bssid[1],
            result.bssid[2],
            result.bssid[3],
            result.bssid[4],
            result.bssid[5]);
        const int values_size = snprintf(
            values,
            sizeof(values),
            " | %d | %u | %s\r\n",
            static_cast<int>(result.rssi),
            static_cast<unsigned int>(result.channel),
            auth_text(result.auth));
        if (bssid_size < 0 || values_size < 0 ||
            static_cast<size_t>(bssid_size) >= sizeof(bssid) ||
            static_cast<size_t>(values_size) >= sizeof(values) ||
            result.ssid_size > sizeof(result.ssid)) {
            return false;
        }

        const size_t row_size =
            static_cast<size_t>(result.ssid_size) +
            sizeof(" | ") - 1U +
            static_cast<size_t>(bssid_size) +
            static_cast<size_t>(values_size);
        if (payload_size > static_cast<size_t>(-1) - row_size) {
            return false;
        }
        payload_size += row_size;
    }
    return true;
}

bool WifiService::write_status_response(
    const at::Command &command,
    at::AtTransport &transport,
    WifiStatus status) const
{
    return transport.write_response(command, wifi_status_text(status));
}

bool WifiService::parse_esp32_bool(const char *text, bool &value)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    value = atoi(text) != 0;
    return true;
}

bool WifiService::parse_uint8(
    const char *text,
    unsigned int minimum,
    unsigned int maximum,
    uint8_t &value)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    char *end = nullptr;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    value = static_cast<uint8_t>(parsed);
    return true;
}

bool WifiService::parse_ipv4(const char *text, uint8_t octets[4])
{
    if (text == nullptr || octets == nullptr) {
        return false;
    }
    unsigned int values[4] = {};
    char tail = '\0';
    if (sscanf(
            text,
            "%u.%u.%u.%u%c",
            &values[0],
            &values[1],
            &values[2],
            &values[3],
            &tail) != 4) {
        return false;
    }
    for (size_t index = 0U; index < 4U; ++index) {
        if (values[index] > 255U) {
            return false;
        }
        octets[index] = static_cast<uint8_t>(values[index]);
    }
    return true;
}

bool WifiService::write_ipv4(
    at::AtTransport &transport,
    const uint8_t octets[4])
{
    char text[16] = {};
    const int size = snprintf(
        text,
        sizeof(text),
        "%u.%u.%u.%u",
        octets[0],
        octets[1],
        octets[2],
        octets[3]);
    return size > 0 &&
        static_cast<size_t>(size) < sizeof(text) &&
        transport.write_cstr(text);
}

bool WifiService::ap_mode_enabled() const
{
    return wifi_mode_ == 2U || wifi_mode_ == 3U;
}

bool WifiService::sta_mode_enabled() const
{
    return wifi_mode_ == 1U || wifi_mode_ == 3U;
}

bool WifiService::sta_connection_active() const
{
    return sta_connect_pending_ || sta_dns_restore_pending_ ||
        status_tracker_.status() == WifiStatus::Connected;
}

const char *WifiService::auth_text(uint8_t auth)
{
    switch (auth) {
    case 0U:
        return "open";
    case 1U:
        return "WEP";
    case 2U:
        return "WPA";
    case 3U:
        return "WPA2";
    case 4U:
        return "WPA+WPA2";
    case 5U:
        return "WPA2-EAP";
    case 6U:
        return "WPA3";
    case 7U:
        return "WPA2+WPA3";
    default:
        return "unknown";
    }
}

} // namespace bridge
