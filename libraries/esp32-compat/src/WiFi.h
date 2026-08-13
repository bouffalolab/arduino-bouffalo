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

    int scanNetworks();
    String SSID(uint8_t networkItem = 0);
    int32_t RSSI(uint8_t networkItem = 0);
    uint8_t *BSSID(uint8_t networkItem = 0);
    String BSSIDstr(uint8_t networkItem = 0);
    uint8_t encryptionType(uint8_t networkItem = 0);
    uint8_t channel(uint8_t networkItem = 0);

    bool config(IPAddress localIP, IPAddress gateway, IPAddress subnet,
                IPAddress dns1 = IPAddress((uint32_t)0),
                IPAddress dns2 = IPAddress((uint32_t)0));

    bool mode(wifi_mode_t mode);
    wifi_mode_t getMode();
    bool setAutoConnect(bool autoConnect);
    bool getAutoConnect();
    bool setAutoReconnect(bool autoReconnect);
    bool getAutoReconnect();

    int begin(const char *ssid, const char *password = nullptr);
    bool disconnect(bool wifiOff = false);
    wl_status_t status();

    String macAddress();
    IPAddress localIP();
    IPAddress gatewayIP();
    IPAddress subnetMask();
    IPAddress dnsIP(uint8_t dnsNo = 0);
    String getHostname();
    bool setHostname(const char *hostname);
    IPAddress localIPv6();
    void enableIpV6();

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

    bool onEvent(WiFiEventCb cb);
    void persistent(bool persistent);
};

extern WiFiClass WiFi;

#endif
