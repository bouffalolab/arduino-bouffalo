/*
WiFiScan.cpp - Bouffalolab Wifi support.

Copyright (c) 2023 BH6BAO <qqwang@bouffalolab.com>
Copyright (c) 2023 Bouffalo Lab Intelligent Technology (Nanjing) Co., Ltd. All rights reserved.

*/

#include "WiFi.h"
#include "WiFiGeneric.h"
#include "WiFiScan.h"

extern "C" {
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <lwip/ip_addr.h>
#include "lwip/err.h"
#include "bl_fw_api.h"
#include "wifi_mgmr_ext.h"
#include "wifi_mgmr.h"
}

bool WiFiScanClass::_scanAsync = false;
uint32_t WiFiScanClass::_scanStarted = 0;
uint32_t WiFiScanClass::_scanTimeout = 10000;
uint16_t WiFiScanClass::_scanCount = 0;
void* WiFiScanClass::_scanResult = 0;

int16_t WiFiScanClass::scanNetworks(bool async, bool show_hidden, bool passive, uint32_t max_ms_per_chan, uint8_t channel, const char *ssid, const uint8_t *bssid)
{
    WiFiScanClass::_scanTimeout = max_ms_per_chan * 20;
    WiFiScanClass::_scanAsync = async;

    int ret = 0;
    // WiFi.init_wifi();

    if (WiFi.get_wifi_status() == ARDUINO_EVENT_MAX) {
        return 0;
    }
    // scanDelete();

    wifi_mgmr_scan_params_t config;
    memset(&config, 0 , sizeof(wifi_mgmr_scan_params_t));

    ret = wifi_mgmr_sta_scan(&config);



    if (ret < 0) {
        printf("scan failed !\r\n");
    } else {
        ret = (int)wifi_mgmr_sta_scanlist_nums_get();
        WiFiScanClass::_scanCount = ret;
    }

    return (int16_t)ret;

}

String WiFiScanClass::SSID(uint8_t i)
{

}

void WiFiScanClass::_scanDone()
{
    (WiFiScanClass::_scanCount) = wifi_mgmr_sta_scanlist_nums_get();
    if(WiFiScanClass::_scanCount) {
        // printf("scan done:%d\r\n", WiFiScanClass::_scanCount);
        // WiFiScanClass::_scanResult = new wifi_ap_record_t[WiFiScanClass::_scanCount];
        // if(!WiFiScanClass::_scanResult || esp_wifi_scan_get_ap_records(&(WiFiScanClass::_scanCount), (wifi_ap_record_t*)_scanResult) != ESP_OK) {
        //     WiFiScanClass::_scanCount = 0;
        // }
    }
    WiFiScanClass::_scanStarted=0; //Reset after a scan is completed for normal behavior
    WiFiGenericClass::setStatusBits(WIFI_SCAN_DONE_BIT);
    WiFiGenericClass::clearStatusBits(WIFI_SCANNING_BIT);
}
