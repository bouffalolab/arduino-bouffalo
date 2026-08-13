#ifndef BL616CL_ESP32_COMPAT_WIFI_H_
#define BL616CL_ESP32_COMPAT_WIFI_H_

#include <Arduino.h>
#include <IPAddress.h>
#include <WString.h>
#include "WiFiType.h"
#include "WiFiGeneric.h"
#include "WiFiUdp.h"
#include "WiFiServer.h"
#include "esp_partition.h"

class WiFiClass : public WiFiGenericClass {
public:
    WiFiClass() {}

    int scanNetworks() { return 0; }
    String SSID(uint8_t networkItem = 0) { (void)networkItem; return String(""); }
    int32_t RSSI(uint8_t networkItem = 0) { (void)networkItem; return 0; }
    uint8_t *BSSID(uint8_t networkItem = 0) { (void)networkItem; return nullptr; }
    String BSSIDstr() { return String(""); }
    uint8_t encryptionType(uint8_t networkItem = 0) { (void)networkItem; return WIFI_AUTH_OPEN; }
    uint8_t channel(uint8_t networkItem = 0) { (void)networkItem; return 0; }

    bool config(IPAddress localIP, IPAddress gateway, IPAddress subnet,
                IPAddress dns1 = IPAddress((uint32_t)0),
                IPAddress dns2 = IPAddress((uint32_t)0))
    {
        (void)localIP; (void)gateway; (void)subnet; (void)dns1; (void)dns2;
        return false;
    }

    bool mode(wifi_mode_t mode) { (void)mode; return false; }
    wifi_mode_t getMode() { return WIFI_MODE_NULL; }
    bool setAutoConnect(bool autoConnect) { (void)autoConnect; return false; }
    bool getAutoConnect() { return false; }
    bool setAutoReconnect(bool autoReconnect) { (void)autoReconnect; return false; }
    bool getAutoReconnect() { return false; }

    int begin(const char *ssid, const char *password = nullptr) { (void)ssid; (void)password; return WL_CONNECT_FAILED; }
    bool disconnect(bool wifiOff = false) { (void)wifiOff; return false; }

    String macAddress() { return String(""); }
    IPAddress localIP() { return IPAddress((uint32_t)0); }
    IPAddress gatewayIP() { return IPAddress((uint32_t)0); }
    IPAddress subnetMask() { return IPAddress((uint32_t)0); }
    IPAddress dnsIP(uint8_t dnsNo = 0) { (void)dnsNo; return IPAddress((uint32_t)0); }
    String getHostname() { return String(""); }
    bool setHostname(const char *hostname) { (void)hostname; return false; }
    IPAddress localIPv6() { return IPAddress((uint32_t)0); }
    void enableIpV6() {}

    bool softAP(const char *ssid, const char *password = nullptr,
                int channel = 1, bool ssidHidden = false, int maxConnection = 5)
    {
        (void)ssid; (void)password; (void)channel; (void)ssidHidden; (void)maxConnection;
        return false;
    }
    bool softAPdisconnect(bool wifiOff = false) { (void)wifiOff; return false; }
    String softAPmacAddress() { return String(""); }
    IPAddress softAPIP() { return IPAddress((uint32_t)0); }
    String softAPSSID() { return String(""); }
    bool softAPConfig(IPAddress localIP, IPAddress gateway, IPAddress subnet)
    {
        (void)localIP; (void)gateway; (void)subnet;
        return false;
    }

    bool onEvent(WiFiEventCb cb) { (void)cb; return false; }
    void persistent(bool persistent) { (void)persistent; }
};

extern WiFiClass WiFi;

#endif
