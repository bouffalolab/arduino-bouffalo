/*
WiFiScan.h - Bouffalolab Wifi support.

Copyright (c) 2023 BH6BAO <qqwang@bouffalolab.com>
Copyright (c) 2023 Bouffalo Lab Intelligent Technology (Nanjing) Co., Ltd. All rights reserved.

*/

#ifndef BOUFFALOLAB_WIFISCAN_H__
#define BOUFFALOLAB_WIFISCAN_H__

#include "WiFiType.h"
#include "WiFiGeneric.h"

// extern "C" {
//     #include "wifi_mgmr_ext.h"
// }

class WiFiScanClass
{
public:

    int16_t scanNetworks(bool async = false,bool show_hidden = false, bool passive = false, uint32_t max_ms_per_chan = 300, uint8_t channel = 0, const char * ssid=nullptr, const uint8_t * bssid=nullptr);

    String SSID(uint8_t networkItem);
    int32_t RSSI(uint8_t networkItem);
    int32_t channel(uint8_t networkItem);
    static void * getScanInfoByIndex(int i) { return _getScanInfoByIndex(i); };

    static void _scanDone();

protected:

    static bool _scanAsync;

    static uint32_t _scanStarted;
    static uint32_t _scanTimeout;
    static uint16_t _scanCount;

    static void* _scanResult;
    // static wifi_mgmr_scan_item_t _scanResult[50];  // max count 50

    static void * _getScanInfoByIndex(int i);
};

#endif // BOUFFALOLAB_WIFISCAN_H__
