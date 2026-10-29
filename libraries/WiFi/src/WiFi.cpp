/* WiFiClass：Wi-Fi 在 wifi_port.c，DNS、ping 在 net_port.c */
#include <stdlib.h>
#include <string.h>

#include "Arduino.h"
#include "WiFi.h"

#include "net_port.h"
#include "wifi_port.h"

#define DISCONNECT_TIMEOUT_MS 3000
#define PING_TIMEOUT_MS       1000
#define PING_INTERVAL_MS      1000
#define SCAN_TIMEOUT_MS       8000 /* 实测全信道扫描 1.5–1.7 s；bridge 要在 WiFiS3 的 10 s 内应答 */
#define AP_TIMEOUT_MS         3000 /* 启动、停止 AP 各自最多等待；bridge 同样要在 10 s 内应答 */

WiFiClass WiFi;

const char *WiFiClass::firmwareVersion()
{
    return WIFI_FIRMWARE_LATEST_VERSION;
}

int WiFiClass::begin(const char *ssid)
{
    return begin(ssid, nullptr);
}

static bool valid_passphrase(const char *pass)
{
    size_t len = pass != nullptr ? strlen(pass) : 0;
    return len == 0 || (len >= 8 && len <= WL_WPA_KEY_MAX_LENGTH + 1); /* 64 个字符为十六进制 PSK */
}

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
    /* 同一网络已连上或正在连接时不重新发起，常见写法是循环调用 begin() 直到连上 */
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

uint8_t WiFiClass::beginAP(const char *ssid)
{
    return beginAP(ssid, "", 1);
}

uint8_t WiFiClass::beginAP(const char *ssid, uint8_t channel)
{
    return beginAP(ssid, "", channel);
}

uint8_t WiFiClass::beginAP(const char *ssid, const char *passphrase)
{
    return beginAP(ssid, passphrase, 1);
}

uint8_t WiFiClass::beginAP(const char *ssid, const char *passphrase, uint8_t channel)
{
    return beginAP(ssid, passphrase, channel, false, 5);
}

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

bool WiFiClass::setAutoReconnect(bool enable)
{
    return wifi_port_set_auto_reconnect(enable) == 0;
}

bool WiFiClass::getAutoReconnect()
{
    return wifi_port_get_auto_reconnect();
}

unsigned long WiFiClass::getTime()
{
    return wifi_port_time();
}

const char *WiFiClass::getHostname()
{
    return wifi_port_init() == 0 ? wifi_port_hostname() : nullptr;
}

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

bool WiFiClass::configTime(const char *server)
{
    return server != nullptr && server[0] != '\0' && strlen(server) <= 253 &&
           wifi_port_init() == 0 && wifi_port_config_time(server) == 0;
}

bool WiFiClass::enableIpV6()
{
    return (mode_ & WIFI_STA) && wifi_port_enable_ipv6() == 0;
}

String WiFiClass::localIPv6()
{
    char address[46];
    wifi_port_ipv6(address, sizeof(address));
    return String(address);
}

uint8_t WiFiClass::stationStatus()
{
    return (uint8_t)wifi_port_sta_state();
}

int WiFiClass::disconnect(void)
{
    return disconnect(false);
}

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

void WiFiClass::end(void)
{
    disconnect();
    wifi_port_ap_stop(AP_TIMEOUT_MS);
    mode_ = WIFI_OFF;
}

uint8_t WiFiClass::status()
{
    return mode_ == WIFI_AP_STA && wifi_port_sta_state() == WIFI_PORT_CONNECTED ? WL_CONNECTED : (uint8_t)wifi_port_state();
}

uint8_t WiFiClass::reasonCode()
{
    uint16_t status, reason;
    wifi_port_last_error(&status, &reason);
    return (uint8_t)status;
}

uint8_t *WiFiClass::macAddress(uint8_t *mac)
{
    if (wifi_port_init() != 0 || wifi_port_mac(mac) != 0) {
        memset(mac, 0, WL_MAC_ADDR_LENGTH);
    }
    return mac;
}

IPAddress WiFiClass::localIP()
{
    if (mode_ == WIFI_AP_STA) {
        return stationIP();
    }
    uint32_t ip, mask, gw;
    wifi_port_ip(&ip, &mask, &gw);
    return IPAddress(ip);
}

IPAddress WiFiClass::subnetMask()
{
    if (mode_ == WIFI_AP_STA) {
        return stationSubnetMask();
    }
    uint32_t ip, mask, gw;
    wifi_port_ip(&ip, &mask, &gw);
    return IPAddress(mask);
}

IPAddress WiFiClass::gatewayIP()
{
    if (mode_ == WIFI_AP_STA) {
        return stationGatewayIP();
    }
    uint32_t ip, mask, gw;
    wifi_port_ip(&ip, &mask, &gw);
    return IPAddress(gw);
}

IPAddress WiFiClass::dnsIP(int n)
{
    return IPAddress(wifi_port_dns(n));
}

void WiFiClass::config(IPAddress local_ip)
{
    IPAddress gw(local_ip[0], local_ip[1], local_ip[2], 1);
    config(local_ip, gw, gw);
}

void WiFiClass::config(IPAddress local_ip, IPAddress dns_server)
{
    config(local_ip, dns_server, IPAddress(local_ip[0], local_ip[1], local_ip[2], 1));
}

void WiFiClass::config(IPAddress local_ip, IPAddress dns_server, IPAddress gateway)
{
    config(local_ip, dns_server, gateway, IPAddress(255, 255, 255, 0));
}

void WiFiClass::config(IPAddress local_ip, IPAddress dns_server, IPAddress gateway, IPAddress subnet)
{
    configSTA(local_ip, dns_server, gateway, subnet);
    softAPConfig(local_ip, gateway, subnet);
}

void WiFiClass::configSTA(IPAddress local_ip, IPAddress dns_server, IPAddress gateway, IPAddress subnet)
{
    wifi_port_ip_config((uint32_t)local_ip, (uint32_t)subnet, (uint32_t)gateway, (uint32_t)dns_server, 0);
}

bool WiFiClass::softAPConfig(IPAddress local_ip, IPAddress gateway, IPAddress subnet)
{
    return wifi_port_ap_config((uint32_t)local_ip, (uint32_t)subnet, (uint32_t)gateway) == 0;
}

IPAddress WiFiClass::stationIP()
{
    uint32_t ip, mask, gw;
    wifi_port_sta_ip(&ip, &mask, &gw);
    return IPAddress(ip);
}

IPAddress WiFiClass::stationSubnetMask()
{
    uint32_t ip, mask, gw;
    wifi_port_sta_ip(&ip, &mask, &gw);
    return IPAddress(mask);
}

IPAddress WiFiClass::stationGatewayIP()
{
    uint32_t ip, mask, gw;
    wifi_port_sta_ip(&ip, &mask, &gw);
    return IPAddress(gw);
}

void WiFiClass::setDNS(IPAddress dns_server1)
{
    setDNS(dns_server1, IPAddress(0, 0, 0, 0));
}

void WiFiClass::setDNS(IPAddress dns_server1, IPAddress dns_server2)
{
    wifi_port_set_dns((uint32_t)dns_server1, (uint32_t)dns_server2);
}

static bool ap_running()
{
    return wifi_port_ap_running();
}

const char *WiFiClass::SSID()
{
    if (mode_ != WIFI_AP_STA && ap_running()) {
        return ap_ssid_;
    }
    return wifi_port_sta_state() == WIFI_PORT_CONNECTED ? ssid_ : "";
}

uint8_t *WiFiClass::BSSID(uint8_t *bssid)
{
    wifi_port_bssid(bssid);
    return bssid;
}

int32_t WiFiClass::RSSI()
{
    return wifi_port_rssi();
}

IPAddress WiFiClass::softAPIP()
{
    uint32_t ip, mask, gw;
    wifi_port_ap_ip(&ip, &mask, &gw);
    return IPAddress(ip);
}

const char *WiFiClass::softAPSSID()
{
    return ap_running() ? ap_ssid_ : "";
}

int WiFiClass::hostByName(const char *aHostname, IPAddress &aResult)
{
    uint32_t addr;

    if (wifi_port_sta_state() != WIFI_PORT_CONNECTED || net_port_resolve(aHostname, &addr) != 0) {
        return 0;
    }
    aResult = IPAddress(addr);
    return 1;
}

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

int WiFiClass::ping(const String &hostname, uint8_t ttl, uint8_t count)
{
    return ping(hostname.c_str(), ttl, count);
}

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

void WiFiClass::setTimeout(unsigned long timeout)
{
    timeout_ = timeout;
}

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

const char *WiFiClass::SSID(uint8_t networkItem)
{
    return networkItem < ap_count_ ? aps_[networkItem].ssid : "";
}

int32_t WiFiClass::RSSI(uint8_t networkItem)
{
    return networkItem < ap_count_ ? aps_[networkItem].rssi : 0;
}

uint8_t WiFiClass::encryptionType(uint8_t networkItem)
{
    return networkItem < ap_count_ ? aps_[networkItem].enc : 0;
}

uint8_t *WiFiClass::BSSID(uint8_t networkItem, uint8_t *bssid)
{
    if (networkItem < ap_count_) {
        memcpy(bssid, aps_[networkItem].bssid, WL_MAC_ADDR_LENGTH);
    } else {
        memset(bssid, 0, WL_MAC_ADDR_LENGTH);
    }
    return bssid;
}

uint8_t WiFiClass::channel(uint8_t networkItem)
{
    return networkItem < ap_count_ ? aps_[networkItem].channel : 0;
}
