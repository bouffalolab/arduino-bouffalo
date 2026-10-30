/* WiFiClass delegates Wi-Fi to wifi_port.c and DNS/ping to net_port.c. */
#include <stdlib.h>
#include <string.h>

#include "Arduino.h"
#include "WiFi.h"

#include "net_port.h"
#include "wifi_port.h"

#define DISCONNECT_TIMEOUT_MS 3000
#define PING_TIMEOUT_MS       1000
#define PING_INTERVAL_MS      1000
#define SCAN_TIMEOUT_MS       8000 /*
 * Measured all-channel scanning takes 1.5-1.7 s; the bridge must answer within the WiFiS3 10 s
 * timeout.
 */
#define AP_TIMEOUT_MS         3000 /* Bound AP start/stop waits separately so the bridge can answer within 10 s. */

WiFiClass WiFi;

/**
 * @brief Return the library's firmware-compatible semantic version string.
 */
const char *WiFiClass::firmwareVersion()
{
    return WIFI_FIRMWARE_LATEST_VERSION;
}

/**
 * @brief Start STA with the supplied credentials and wait for connection/addressing within the
 * configured timeout.
 */
int WiFiClass::begin(const char *ssid)
{
    return begin(ssid, nullptr);
}

/**
 * @brief Validate an empty/open passphrase, an 8-63 character passphrase or a 64-digit
 * hexadecimal PSK.
 */
static bool valid_passphrase(const char *pass)
{
    size_t len = pass != nullptr ? strlen(pass) : 0;
    return len == 0 || (len >= 8 && len <= WL_WPA_KEY_MAX_LENGTH + 1); /* A 64-character value is a hexadecimal PSK. */
}

/**
 * @brief Start STA with the supplied credentials and wait for connection/addressing within the
 * configured timeout.
 */
int WiFiClass::begin(const char *ssid, const char *passphrase)
{
    if (wifi_port_init() != 0) {
        return WL_NO_MODULE;
    }
    size_t len = ssid != nullptr ? strlen(ssid) : 0;
    if (len == 0 || len > WL_SSID_MAX_LENGTH || !valid_passphrase(passphrase)) {
        return WL_CONNECT_FAILED;
    }
    if (mode_ != WIFI_AP_STA) {
        mode(WIFI_STA);
    }
    const char *pass = passphrase != nullptr ? passphrase : "";
    bool same = strcmp(ssid, ssid_) == 0 && strcmp(pass, pass_) == 0;
    /*
 * Do not restart an active or pending connection to the same network; sketches commonly repeat
 * begin() until connected.
 */
    if (!(same && (wifi_port_sta_state() == WIFI_PORT_CONNECTED || wifi_port_connecting()))) {
        if (wifi_port_sta_state() == WIFI_PORT_CONNECTED || wifi_port_connecting()) {
            wifi_port_disconnect(DISCONNECT_TIMEOUT_MS);
        }
        strcpy(ssid_, ssid);
        strcpy(pass_, pass);
        if (wifi_port_connect(ssid_, pass_) != 0) {
            return WL_CONNECT_FAILED;
        }
    }
    unsigned long start = millis();
    while (wifi_port_connecting() && millis() - start < timeout_) {
        delay(10);
    }
    return stationStatus();
}

/**
 * @brief Start an access point with the selected credentials, channel and association options.
 */
uint8_t WiFiClass::beginAP(const char *ssid)
{
    return beginAP(ssid, "", 1);
}

/**
 * @brief Start an access point with the selected credentials, channel and association options.
 */
uint8_t WiFiClass::beginAP(const char *ssid, uint8_t channel)
{
    return beginAP(ssid, "", channel);
}

/**
 * @brief Start an access point with the selected credentials, channel and association options.
 */
uint8_t WiFiClass::beginAP(const char *ssid, const char *passphrase)
{
    return beginAP(ssid, passphrase, 1);
}

/**
 * @brief Start an access point with the selected credentials, channel and association options.
 */
uint8_t WiFiClass::beginAP(const char *ssid, const char *passphrase, uint8_t channel)
{
    return beginAP(ssid, passphrase, channel, false, 5);
}

/**
 * @brief Start an access point with the selected credentials, channel and association options.
 */
uint8_t WiFiClass::beginAP(const char *ssid, const char *passphrase, uint8_t channel, bool hidden, uint8_t maxConnections)
{
    size_t len = ssid != nullptr ? strlen(ssid) : 0;
    size_t pass_len = passphrase != nullptr ? strlen(passphrase) : 0;
    if (wifi_port_init() != 0 || len == 0 || len > WL_SSID_MAX_LENGTH ||
        (pass_len != 0 && (pass_len < 8 || pass_len > WL_WPA_KEY_MAX_LENGTH)) || channel < 1 || channel > 13 ||
        maxConnections < 1 || maxConnections > 5) {
        return WL_AP_FAILED;
    }
    if (mode_ != WIFI_AP_STA) {
        mode(WIFI_AP);
    }
    strcpy(ap_ssid_, ssid);
    return wifi_port_ap_start(ap_ssid_, passphrase, channel, hidden, maxConnections, AP_TIMEOUT_MS) == 0 ? WL_AP_LISTENING : WL_AP_FAILED;
}

/**
 * @brief Select the enabled STA/AP interface set while preserving its requested lifecycle
 * policy.
 */
bool WiFiClass::mode(WiFiMode value)
{
    if (value < WIFI_OFF || value > WIFI_AP_STA || (value != WIFI_OFF && wifi_port_init() != 0)) {
        return false;
    }
    if ((mode_ & WIFI_STA) && !(value & WIFI_STA)) {
        wifi_port_disconnect(DISCONNECT_TIMEOUT_MS);
    }
    if ((mode_ & WIFI_AP) && !(value & WIFI_AP)) {
        wifi_port_ap_stop(AP_TIMEOUT_MS);
    }
    mode_ = value;
    return true;
}

/**
 * @brief Enable or disable reconnect after an unexpected STA disconnection.
 */
bool WiFiClass::setAutoReconnect(bool enable)
{
    return wifi_port_set_auto_reconnect(enable) == 0;
}

/**
 * @brief Return the current unexpected-disconnect reconnect policy.
 */
bool WiFiClass::getAutoReconnect()
{
    return wifi_port_get_auto_reconnect();
}

/**
 * @brief Return the platform UTC epoch seconds, or zero before time synchronization.
 */
unsigned long WiFiClass::getTime()
{
    return wifi_port_time();
}

/**
 * @brief Return the current RAM-resident DHCP hostname.
 */
const char *WiFiClass::getHostname()
{
    return wifi_port_init() == 0 ? wifi_port_hostname() : nullptr;
}

/**
 * @brief Validate and set the RAM-resident DHCP hostname for subsequent network use.
 */
bool WiFiClass::setHostname(const char *hostname)
{
    size_t len = hostname != nullptr ? strlen(hostname) : 0;
    if (len == 0 || len > 31 || hostname[0] == '-' || hostname[len - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        char c = hostname[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return wifi_port_init() == 0 && wifi_port_set_hostname(hostname) == 0;
}

/**
 * @brief Select the SNTP server and request platform time synchronization.
 */
bool WiFiClass::configTime(const char *server)
{
    return server != nullptr && server[0] != '\0' && strlen(server) <= 253 &&
           wifi_port_init() == 0 && wifi_port_config_time(server) == 0;
}

/**
 * @brief Enable the supported STA link-local IPv6 address.
 */
bool WiFiClass::enableIpV6()
{
    return (mode_ & WIFI_STA) && wifi_port_enable_ipv6() == 0;
}

/**
 * @brief Return the supported STA link-local IPv6 address as text.
 */
String WiFiClass::localIPv6()
{
    char address[46];
    wifi_port_ipv6(address, sizeof(address));
    return String(address);
}

/**
 * @brief Return STA status independently of an active AP.
 */
uint8_t WiFiClass::stationStatus()
{
    return (uint8_t)wifi_port_sta_state();
}

/**
 * @brief Disconnect STA, optionally removing its mode bit while preserving AP.
 */
int WiFiClass::disconnect(void)
{
    return disconnect(false);
}

/**
 * @brief Disconnect STA, optionally removing its mode bit while preserving AP.
 */
int WiFiClass::disconnect(bool wifiOff)
{
    if (wifi_port_init() == 0) {
        wifi_port_disconnect(DISCONNECT_TIMEOUT_MS);
    }
    if (wifiOff) {
        mode_ = (WiFiMode)(mode_ & ~WIFI_STA);
    }
    return WL_DISCONNECTED;
}

/**
 * @brief Stop AP, optionally removing its mode bit while preserving STA.
 */
bool WiFiClass::softAPdisconnect(bool wifiOff)
{
    if (mode_ & WIFI_AP) {
        wifi_port_ap_stop(AP_TIMEOUT_MS);
    }
    if (wifiOff) {
        mode_ = (WiFiMode)(mode_ & ~WIFI_AP);
    }
    return !wifi_port_ap_running();
}

/**
 * @brief Stop both Wi-Fi interfaces and clear the selected mode state.
 */
void WiFiClass::end(void)
{
    disconnect();
    wifi_port_ap_stop(AP_TIMEOUT_MS);
    mode_ = WIFI_OFF;
}

/**
 * @brief Return the mode-selected Wi-Fi status code.
 */
uint8_t WiFiClass::status()
{
    return mode_ == WIFI_AP_STA && wifi_port_sta_state() == WIFI_PORT_CONNECTED ? WL_CONNECTED : (uint8_t)wifi_port_state();
}

/**
 * @brief Return the latest wl80211 connection/disconnection status code.
 */
uint8_t WiFiClass::reasonCode()
{
    uint16_t status, reason;
    wifi_port_last_error(&status, &reason);
    return (uint8_t)status;
}

/**
 * @brief Fill the caller's MAC buffer from the mode-selected interface, or zeros on failure.
 */
uint8_t *WiFiClass::macAddress(uint8_t *mac)
{
    if (wifi_port_init() != 0 || wifi_port_mac(mac) != 0) {
        memset(mac, 0, WL_MAC_ADDR_LENGTH);
    }
    return mac;
}

/**
 * @brief Return the mode-selected local address.
 */
IPAddress WiFiClass::localIP()
{
    if (mode_ == WIFI_AP_STA) {
        return stationIP();
    }
    uint32_t ip, mask, gw;
    wifi_port_ip(&ip, &mask, &gw);
    return IPAddress(ip);
}

/**
 * @brief Return the mode-selected network mask.
 */
IPAddress WiFiClass::subnetMask()
{
    if (mode_ == WIFI_AP_STA) {
        return stationSubnetMask();
    }
    uint32_t ip, mask, gw;
    wifi_port_ip(&ip, &mask, &gw);
    return IPAddress(mask);
}

/**
 * @brief Return the mode-selected gateway address.
 */
IPAddress WiFiClass::gatewayIP()
{
    if (mode_ == WIFI_AP_STA) {
        return stationGatewayIP();
    }
    uint32_t ip, mask, gw;
    wifi_port_ip(&ip, &mask, &gw);
    return IPAddress(gw);
}

/**
 * @brief Return the selected DNS server address.
 */
IPAddress WiFiClass::dnsIP(int n)
{
    return IPAddress(wifi_port_dns(n));
}

/**
 * @brief Apply WiFiS3-compatible static addressing and defaults; zero address restores DHCP.
 */
void WiFiClass::config(IPAddress local_ip)
{
    IPAddress gw(local_ip[0], local_ip[1], local_ip[2], 1);
    config(local_ip, gw, gw);
}

/**
 * @brief Apply WiFiS3-compatible static addressing and defaults; zero address restores DHCP.
 */
void WiFiClass::config(IPAddress local_ip, IPAddress dns_server)
{
    config(local_ip, dns_server, IPAddress(local_ip[0], local_ip[1], local_ip[2], 1));
}

/**
 * @brief Apply WiFiS3-compatible static addressing and defaults; zero address restores DHCP.
 */
void WiFiClass::config(IPAddress local_ip, IPAddress dns_server, IPAddress gateway)
{
    config(local_ip, dns_server, gateway, IPAddress(255, 255, 255, 0));
}

/**
 * @brief Apply WiFiS3-compatible static addressing and defaults; zero address restores DHCP.
 */
void WiFiClass::config(IPAddress local_ip, IPAddress dns_server, IPAddress gateway, IPAddress subnet)
{
    configSTA(local_ip, dns_server, gateway, subnet);
    softAPConfig(local_ip, gateway, subnet);
}

/**
 * @brief Configure STA addressing without changing AP addressing.
 */
void WiFiClass::configSTA(IPAddress local_ip, IPAddress dns_server, IPAddress gateway, IPAddress subnet)
{
    wifi_port_ip_config((uint32_t)local_ip, (uint32_t)subnet, (uint32_t)gateway, (uint32_t)dns_server, 0);
}

/**
 * @brief Configure the AP address and supported subnet, restarting DHCP when necessary.
 */
bool WiFiClass::softAPConfig(IPAddress local_ip, IPAddress gateway, IPAddress subnet)
{
    return wifi_port_ap_config((uint32_t)local_ip, (uint32_t)subnet, (uint32_t)gateway) == 0;
}

/**
 * @brief Return the STA address independently of AP mode.
 */
IPAddress WiFiClass::stationIP()
{
    uint32_t ip, mask, gw;
    wifi_port_sta_ip(&ip, &mask, &gw);
    return IPAddress(ip);
}

/**
 * @brief Return the STA subnet mask independently of AP mode.
 */
IPAddress WiFiClass::stationSubnetMask()
{
    uint32_t ip, mask, gw;
    wifi_port_sta_ip(&ip, &mask, &gw);
    return IPAddress(mask);
}

/**
 * @brief Return the STA gateway independently of AP mode.
 */
IPAddress WiFiClass::stationGatewayIP()
{
    uint32_t ip, mask, gw;
    wifi_port_sta_ip(&ip, &mask, &gw);
    return IPAddress(gw);
}

/**
 * @brief Update nonzero DNS entries using the WiFiS3-compatible DHCP/static policy.
 */
void WiFiClass::setDNS(IPAddress dns_server1)
{
    setDNS(dns_server1, IPAddress(0, 0, 0, 0));
}

/**
 * @brief Update nonzero DNS entries using the WiFiS3-compatible DHCP/static policy.
 */
void WiFiClass::setDNS(IPAddress dns_server1, IPAddress dns_server2)
{
    wifi_port_set_dns((uint32_t)dns_server1, (uint32_t)dns_server2);
}

/**
 * @brief Report whether the platform AP interface is active.
 */
static bool ap_running()
{
    return wifi_port_ap_running();
}

/**
 * @brief Return the selected current or indexed scan-result SSID.
 */
const char *WiFiClass::SSID()
{
    if (mode_ != WIFI_AP_STA && ap_running()) {
        return ap_ssid_;
    }
    return wifi_port_sta_state() == WIFI_PORT_CONNECTED ? ssid_ : "";
}

/**
 * @brief Copy the current or indexed scan-result BSSID into the caller's six-byte buffer.
 */
uint8_t *WiFiClass::BSSID(uint8_t *bssid)
{
    wifi_port_bssid(bssid);
    return bssid;
}

/**
 * @brief Return current or indexed scan-result signal strength in dBm.
 */
int32_t WiFiClass::RSSI()
{
    return wifi_port_rssi();
}

/**
 * @brief Return the active AP address, or 0.0.0.0 when stopped.
 */
IPAddress WiFiClass::softAPIP()
{
    uint32_t ip, mask, gw;
    wifi_port_ap_ip(&ip, &mask, &gw);
    return IPAddress(ip);
}

/**
 * @brief Return the active AP SSID, or an empty string when stopped.
 */
const char *WiFiClass::softAPSSID()
{
    return ap_running() ? ap_ssid_ : "";
}

/**
 * @brief Resolve a hostname to IPv4 and return 1 on success.
 */
int WiFiClass::hostByName(const char *aHostname, IPAddress &aResult)
{
    uint32_t addr;

    if (wifi_port_sta_state() != WIFI_PORT_CONNECTED || net_port_resolve(aHostname, &addr) != 0) {
        return 0;
    }
    aResult = IPAddress(addr);
    return 1;
}

/**
 * @brief Send bounded ICMP echo probes and return mean successful RTT or a WL_PING error.
 */
int WiFiClass::ping(IPAddress ip, uint8_t ttl, uint8_t count)
{
    static uint16_t seq;
    uint32_t total = 0;
    int ok = 0;
    int err = WL_PING_TIMEOUT;

    if (!wifi_port_net_up()) {
        return WL_PING_ERROR;
    }
    for (uint8_t i = 0; i < count; i++) {
        if (i > 0) {
            delay(PING_INTERVAL_MS);
        }
        int rtt = net_port_ping((uint32_t)ip, ttl, ++seq, PING_TIMEOUT_MS);
        if (rtt >= 0) {
            total += rtt;
            ok++;
        } else if (rtt == WL_PING_ERROR) {
            err = WL_PING_ERROR;
        }
    }
    return ok > 0 ? (int)(total / ok) : err;
}

/**
 * @brief Send bounded ICMP echo probes and return mean successful RTT or a WL_PING error.
 */
int WiFiClass::ping(const String &hostname, uint8_t ttl, uint8_t count)
{
    return ping(hostname.c_str(), ttl, count);
}

/**
 * @brief Send bounded ICMP echo probes and return mean successful RTT or a WL_PING error.
 */
int WiFiClass::ping(const char *host, uint8_t ttl, uint8_t count)
{
    IPAddress ip;

    if (wifi_port_sta_state() != WIFI_PORT_CONNECTED) {
        return WL_PING_ERROR;
    }
    if (!hostByName(host, ip)) {
        return WL_PING_UNKNOWN_HOST;
    }
    return ping(ip, ttl, count);
}

/**
 * @brief Set the Wi-Fi association/address wait timeout in milliseconds.
 */
void WiFiClass::setTimeout(unsigned long timeout)
{
    timeout_ = timeout;
}

/**
 * @brief Run a bounded scan and retain the strongest supported number of results.
 */
int8_t WiFiClass::scanNetworks()
{
    ap_count_ = 0;
    if (wifi_port_init() != 0) {
        return -1;
    }
    if (aps_ == nullptr) {
        aps_ = static_cast<struct wifi_port_ap *>(malloc(WL_NETWORKS_LIST_MAXNUM * sizeof(*aps_)));
        if (aps_ == nullptr) {
            return -1;
        }
    }
    int n = wifi_port_scan(aps_, WL_NETWORKS_LIST_MAXNUM, SCAN_TIMEOUT_MS);
    if (n < 0) {
        return -1;
    }
    ap_count_ = (uint8_t)n;
    return (int8_t)n;
}

/**
 * @brief Return the selected current or indexed scan-result SSID.
 */
const char *WiFiClass::SSID(uint8_t networkItem)
{
    return networkItem < ap_count_ ? aps_[networkItem].ssid : "";
}

/**
 * @brief Return current or indexed scan-result signal strength in dBm.
 */
int32_t WiFiClass::RSSI(uint8_t networkItem)
{
    return networkItem < ap_count_ ? aps_[networkItem].rssi : 0;
}

/**
 * @brief Return the WiFiS3 encryption category for the selected scan result.
 */
uint8_t WiFiClass::encryptionType(uint8_t networkItem)
{
    return networkItem < ap_count_ ? aps_[networkItem].enc : 0;
}

/**
 * @brief Copy the current or indexed scan-result BSSID into the caller's six-byte buffer.
 */
uint8_t *WiFiClass::BSSID(uint8_t networkItem, uint8_t *bssid)
{
    if (networkItem < ap_count_) {
        memcpy(bssid, aps_[networkItem].bssid, WL_MAC_ADDR_LENGTH);
    } else {
        memset(bssid, 0, WL_MAC_ADDR_LENGTH);
    }
    return bssid;
}

/**
 * @brief Return the selected scan result's channel, or zero for an invalid index.
 */
uint8_t WiFiClass::channel(uint8_t networkItem)
{
    return networkItem < ap_count_ ? aps_[networkItem].channel : 0;
}
