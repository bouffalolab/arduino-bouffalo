/* WiFiClass：Wi-Fi 在 wifi_port.c，DNS、ping 在 net_port.c */
#include <string.h>

#include "Arduino.h"
#include "WiFi.h"

#include "net_port.h"
#include "wifi_port.h"

#define DISCONNECT_TIMEOUT_MS 3000
#define PING_TIMEOUT_MS       1000
#define PING_INTERVAL_MS      1000

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
    const char *pass = passphrase != nullptr ? passphrase : "";
    bool same = strcmp(ssid, ssid_) == 0 && strcmp(pass, pass_) == 0;
    /* 同一网络已连上或正在连接时不重新发起，常见写法是循环调用 begin() 直到连上 */
    if (!(same && (wifi_port_state() == WIFI_PORT_CONNECTED || wifi_port_connecting()))) {
        if (wifi_port_state() == WIFI_PORT_CONNECTED || wifi_port_connecting()) {
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
    return status();
}

int WiFiClass::disconnect(void)
{
    if (wifi_port_init() == 0) {
        wifi_port_disconnect(DISCONNECT_TIMEOUT_MS);
    }
    return WL_DISCONNECTED;
}

void WiFiClass::end(void)
{
    disconnect();
}

uint8_t WiFiClass::status()
{
    return (uint8_t)wifi_port_state();
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
    uint32_t ip, mask, gw;
    wifi_port_ip(&ip, &mask, &gw);
    return IPAddress(ip);
}

IPAddress WiFiClass::subnetMask()
{
    uint32_t ip, mask, gw;
    wifi_port_ip(&ip, &mask, &gw);
    return IPAddress(mask);
}

IPAddress WiFiClass::gatewayIP()
{
    uint32_t ip, mask, gw;
    wifi_port_ip(&ip, &mask, &gw);
    return IPAddress(gw);
}

IPAddress WiFiClass::dnsIP(int n)
{
    return IPAddress(wifi_port_dns(n));
}

const char *WiFiClass::SSID()
{
    return wifi_port_state() == WIFI_PORT_CONNECTED ? ssid_ : "";
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

int WiFiClass::hostByName(const char *aHostname, IPAddress &aResult)
{
    uint32_t addr;

    if (wifi_port_state() != WIFI_PORT_CONNECTED || net_port_resolve(aHostname, &addr) != 0) {
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

    if (wifi_port_state() != WIFI_PORT_CONNECTED) {
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

    if (wifi_port_state() != WIFI_PORT_CONNECTED) {
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
