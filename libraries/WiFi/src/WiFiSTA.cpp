/*
WiFiSTA.cpp - Bouffalolab Wifi support.

Copyright (c) 2023 BH6BAO <qqwang@bouffalolab.com>
Copyright (c) 2023 Bouffalo Lab Intelligent Technology (Nanjing) Co., Ltd. All rights reserved.

*/

#include "WiFi.h"
#include "WiFiGeneric.h"
#include "WiFiSTA.h"

extern "C" {
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include "lwip/err.h"
#include "lwip/dns.h"

#include "bl_fw_api.h"
#include "wifi_mgmr_ext.h"
#include "wifi_mgmr.h"
#include "bl616_glb.h"
#include "bl616_ef_ctrl.h"
#include "rfparam_adapter.h"

#include "tcpip.h"
#include "mem.h"
// #include "shell.h"
#include "log.h"

}
#define DBG_TAG "WiFi STA"
static wl_status_t _sta_status = WL_NO_SHIELD;
extern arduino_event_id_t wifi_event_id;

wifi_mgmr_t wifi_sta_mgmr;

uint8_t WiFiSTAClass::status()
{
    // printf("wifi event id:%d, status:%d\r\n", wifi_event_id, _sta_status);
    switch (wifi_event_id)
    {
        case ARDUINO_EVENT_WIFI_READY:{
            _sta_status = WL_IDLE_STATUS;
            return _sta_status;
        } break;

        // case ARDUINO_EVENT_WIFI_STA_CONNECTED:{
        //     _sta_status = WL_CONNECTED;
        //     return _sta_status;
        // } break;

        case ARDUINO_EVENT_WIFI_STA_GOT_IP : {
            _sta_status = WL_CONNECTED;
            return _sta_status;
        } break;

        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED : {
            _sta_status = WL_DISCONNECTED;
            return _sta_status;
        } break;
        default:
            break;
    }
    // return _sta_status;
}

void WiFiSTAClass::begin(const char *ssid, const char *password)
{
    WiFi.init_wifi();
    // LOG_I("WiFi init %d\r\n", _sta_status);

    while(status() != WL_IDLE_STATUS) {
        delay(WL_DELAY_START_CONNECTION);
        LOG_I("wait wifi init done! %d\r\n", _sta_status);
    }

    int pmf_cfg = 1;
    // uint8_t channel_index = 0;
    uint16_t freq = 0;
    int use_dhcp = 1;

    // freq = phy_channel_to_freq(PHY_BAND_2G4, channel_index);

    wifi_sta_connect((char *)ssid, (char *)password, NULL, NULL, pmf_cfg, freq, freq, use_dhcp);

    return ;
}

void WiFiSTAClass::begin(const char *ssid, const char *password, int32_t channel, const uint8_t *bssid, bool connect)
{
    WiFi.init_wifi();

    while(status() != WL_IDLE_STATUS) {
        delay(WL_DELAY_START_CONNECTION);
        LOG_I("wait wifi init done! %d\r\n", _sta_status);
    }

    if (!ssid || *ssid == 0x00 || strlen(ssid) > 32) {
        LOG_E("SSID too long or missing!");
        // return WL_CONNECT_FAILED;
    }

    if (password && strlen(password) > 64) {
        LOG_E("password too long!");
        // return WL_CONNECT_FAILED;
    }

    int pmf_cfg = 1;
    // uint8_t channel_index = 0;
    uint16_t freq = 0;
    int use_dhcp = 1;

    freq = WiFi.wifi_channel_to_freq(WiFi_BAND_2G4, channel);

    wifi_sta_connect((char *)ssid, (char *)password, (char *)bssid, NULL, pmf_cfg, freq, freq, use_dhcp);

    return;
}

void WiFiSTAClass::begin(char *ssid, char *password, int32_t channel, const uint8_t *bssid, bool connect)
{
    begin((const char*) ssid, (const char*)password, channel, bssid, connect);
}


void WiFiSTAClass::disconnect()
{
    wifi_sta_disconnect();
}

bool WiFiSTAClass::config(IPAddress local_ip, IPAddress gateway, IPAddress subnet, IPAddress dns1, IPAddress dns2)
{
    if(!WiFi.init_wifi())
    {
        return false;
    }

}

String WiFiSTAClass::SSID() const
{
    wifi_mgmr_connect_ind_stat_info_t wifi_stat;

    if (wifi_mgmr_sta_connect_ind_stat_get(&wifi_stat) != 0) {
        LOG_E("Get SSID failed!");
    }

    return String(reinterpret_cast<char*>(wifi_stat.ssid));
}

String WiFiSTAClass::psk() const
{
    wifi_mgmr_connect_ind_stat_info_t wifi_stat;

    if (wifi_mgmr_sta_connect_ind_stat_get(&wifi_stat) != 0) {
        LOG_E("Get PSK failed!");
        return String();
    }

    return String(reinterpret_cast<char*>(wifi_stat.passphr));
}

uint8_t * WiFiSTAClass::BSSID(void)
{
    static uint8_t bssid[6];
    wifi_mgmr_connect_ind_stat_info_t wifi_stat;

    if (wifi_mgmr_sta_connect_ind_stat_get(&wifi_stat) != 0) {
        LOG_E("Get BSSID failed!");
        return NULL;
    }
    memcpy(bssid, wifi_stat.bssid, 6);
    return (reinterpret_cast<uint8_t*>(bssid));
}

String WiFiSTAClass::BSSIDstr(void)
{
    uint8_t *bssid = BSSID();
    if (!bssid) {
        return String();
    }
    char mac[18] = {0};
    sprintf(mac, "%02X:%02X:%02X:%02X:%02X:%02X", bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    return String(mac);
}

int8_t WiFiSTAClass::RSSI()
{
    int rssi = 0;
    // char rssi_str[33];

    if (wifi_mgmr_sta_rssi_get(&rssi) != 0) {
        // LOG_E("Get rssi error!");
        return (int8_t)rssi;
    }

    return (int8_t)rssi;

    // snprintf(rssi_str, sizeof(rssi_str), "%d", rssi);
    // return String(reinterpret_cast<char*>(rssi_str));
}

IPAddress WiFiSTAClass::localIP()
{
    uint32_t ip_addr = 0;

    if (wifi_sta_ip4_addr_get(&ip_addr, NULL, NULL, NULL) != 0)
    {
        LOG_E("Netif get ip failed!");
        return IPAddress();
    }
    return IPAddress(ip_addr);
}

uint8_t * WiFiSTAClass::macAddress(uint8_t *mac)
{
    if (status() != WL_NO_SHIELD) {
        wifi_mgmr_sta_mac_get(mac);
    } else {
        EF_Ctrl_Read_MAC_Address(mac);
    }
    return mac;
}

String WiFiSTAClass::macAddress(void)
{
    uint8_t mac[6];
    char macStr[18] = {0};
    if (status() != WL_NO_SHIELD) {
        wifi_mgmr_sta_mac_get(mac);
    } else {
        EF_Ctrl_Read_MAC_Address(mac);
    }
    sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(macStr);
}

