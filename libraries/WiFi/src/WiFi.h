/*
WiFi.h - Bouffalolab Wifi support.

Copyright (c) 2023 BH6BAO <qqwang@bouffalolab.com>
Copyright (c) 2023 Bouffalo Lab Intelligent Technology (Nanjing) Co., Ltd. All rights reserved.

*/

#ifndef WiFi_h
#define WiFi_h

#include "Arduino.h"
#include <stdint.h>

#include "Print.h"
#include "IPAddress.h"

#include "WiFiType.h"
#include "WiFiSTA.h"
#include "WiFiGeneric.h"
#include "WiFiScan.h"

// 5 secs of delay to have the connection established
#define WL_DELAY_START_CONNECTION 5000


class WiFiClass : public WiFiGenericClass, public WiFiSTAClass, public WiFiScanClass
{
private:
    // static void init(void);
    bool prov_enable;
    /* data */
public:
    WiFiClass()
    {
        prov_enable = false;
    }

    using WiFiSTAClass::SSID;
    using WiFiSTAClass::RSSI;
    using WiFiSTAClass::BSSID;
    using WiFiSTAClass::BSSIDstr;

    using WiFiScanClass::SSID;
    // using WiFiScanClass::encryptionType;
    using WiFiScanClass::RSSI;
    // using WiFiScanClass::BSSID;
    // using WiFiScanClass::BSSIDstr;
    using WiFiScanClass::channel;

public:
    void enableProv(bool status);
    bool isProvEnabled();

};

extern WiFiClass WiFi;

#endif // WiFi_h
