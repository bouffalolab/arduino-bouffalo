/*
WiFiSTA.h - Bouffalolab Wifi support.

Copyright (c) 2023 BH6BAO <qqwang@bouffalolab.com>
Copyright (c) 2023 Bouffalo Lab Intelligent Technology (Nanjing) Co., Ltd. All rights reserved.

*/

#ifndef BOUFFALOLAB_WIFISTA_H__
#define BOUFFALOLAB_WIFISTA_H__

#include "WiFiType.h"
#include "WiFiGeneric.h"

class WiFiSTAClass
{
    // WiFi STA Function
public:

    void begin(const char *ssid, const char *password = NULL);
    void begin(const char *ssid, const char *password = NULL, int32_t channel = 0, const uint8_t *bssid = NULL, bool connect = true);
    void begin(char *ssid, char *password = NULL, int32_t channel = 0, const uint8_t *bssid = NULL, bool connect = true);
    // void begin();

    bool config(IPAddress local_ip, IPAddress gateway, IPAddress subnet, IPAddress dns1 = (uint32_t)0x00000000, IPAddress dns2 = (uint32_t)0x00000000);

    void disconnect();

    // STA network info
    IPAddress localIP();

    uint8_t * macAddress(uint8_t *mac);
    String macAddress();

    // STA WiFi info
    uint8_t status();
    String SSID() const;
    String psk() const;

    uint8_t * BSSID();
    String BSSIDstr();

    int8_t RSSI();

};


#endif // BOUFFALOLAB_WIFISTA_H__
