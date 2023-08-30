/*
WiFi.cpp - WiFi library for BouffaloLab

Copyright (c) 2023 BH6BAO <qqwang@bouffalolab.com>
Copyright (c) 2023 Bouffalo Lab Intelligent Technology (Nanjing) Co., Ltd. All rights reserved.

*/

#include "WiFi.h"

static wl_status_t _sta_status = WL_NO_SHIELD;

extern "C" {
    #include <stdint.h>
    #include <stdbool.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <inttypes.h>
    #include <string.h>
    #include "bl_fw_api.h"
    #include "wifi_mgmr_ext.h"
    #include "wifi_mgmr.h"
    #include "bl616_glb.h"
    #include "rfparam_adapter.h"

    #include "tcpip.h"
    #include "mem.h"
    // #include "shell.h"
    #include "log.h"

static wifi_conf_t conf = {
    .country_code = "CN",
};

#define DBG_TAG "WiFi Lib"
#define WIFI_STACK_SIZE  (1536)
#define TASK_PRIORITY_FW (16)

// static struct bflb_device_s *uart0;
static TaskHandle_t wifi_fw_task;
// extern void shell_init_with_task(struct bflb_device_s *shell);

void wifi_event_handler(uint32_t code)
{
    switch (code) {
        case CODE_WIFI_ON_INIT_DONE: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_INIT_DONE\r\n", __func__);
            wifi_mgmr_init(&conf);
        } break;
        case CODE_WIFI_ON_MGMR_DONE: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_MGMR_DONE\r\n", __func__);
            _sta_status = WL_IDLE_STATUS;
        } break;
        case CODE_WIFI_ON_SCAN_DONE: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_SCAN_DONE\r\n", __func__);
            wifi_mgmr_sta_scanlist();
            _sta_status = WL_SCAN_COMPLETED;
        } break;
        case CODE_WIFI_ON_CONNECTED: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_CONNECTED\r\n", __func__);
            void mm_sec_keydump();
            mm_sec_keydump();
            _sta_status = WL_CONNECTED;
        } break;
        case CODE_WIFI_ON_GOT_IP: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_GOT_IP\r\n", __func__);
            LOG_I("[SYS] Memory left is %d Bytes\r\n", kfree_size());
            _sta_status = WL_CONNECTED;
        } break;
        case CODE_WIFI_ON_DISCONNECT: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_DISCONNECT\r\n", __func__);
            if (_sta_status == WL_CONNECTED) {
                _sta_status = WL_DISCONNECTED;
            } else if ((_sta_status == WL_IDLE_STATUS) || (_sta_status == WL_SCAN_COMPLETED)) {
                _sta_status == WL_CONNECT_FAILED;
            }
        } break;
        case CODE_WIFI_ON_AP_STARTED: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_AP_STARTED\r\n", __func__);
        } break;
        case CODE_WIFI_ON_AP_STOPPED: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_AP_STOPPED\r\n", __func__);
        } break;
        case CODE_WIFI_ON_AP_STA_ADD: {
            LOG_I("[EVENT] [AP] [ADD] %lld\r\n", xTaskGetTickCount());
        } break;
        case CODE_WIFI_ON_AP_STA_DEL: {
            LOG_I("[EVENT] [AP] [DEL] %lld\r\n", xTaskGetTickCount());
        } break;
        default: {
            LOG_I("[EVENT] Unknown code %u \r\n", code);
        }
    }
}

int wifi_start_firmware_task(void)
{
    LOG_I("Starting wifi ...\r\n");

    /* enable wifi clock */

    GLB_PER_Clock_UnGate(GLB_AHB_CLOCK_IP_WIFI_PHY | GLB_AHB_CLOCK_IP_WIFI_MAC_PHY | GLB_AHB_CLOCK_IP_WIFI_PLATFORM);
    GLB_AHB_MCU_Software_Reset(GLB_AHB_MCU_SW_WIFI);

    if (0 != rfparam_init(0, NULL, 0)) {
        LOG_I("PHY RF init failed!\r\n");
        return 0;
    }

    LOG_I("PHY RF init success!\r\n");

    /* Enable wifi irq */
    extern void interrupt0_handler(void);

    bflb_irq_attach(WIFI_IRQn, (irq_callback)interrupt0_handler, NULL);
    bflb_irq_enable(WIFI_IRQn);

    xTaskCreate(wifi_main, (char *)"fw", WIFI_STACK_SIZE, NULL, TASK_PRIORITY_FW, &wifi_fw_task);

    return 0;
}

}

void WiFiClass::init(void)
{
    // uart0 = bflb_device_get_by_name("uart0");
    // shell_init_with_task(uart0);

    tcpip_init(NULL, NULL);
    wifi_start_firmware_task();
}

void WiFiClass::begin(const char *ssid, const char *password)
{
    WiFiClass::init();
    // LOG_I("WiFi init %d\r\n", _sta_status);

    while(status() != WL_IDLE_STATUS) {
        delay(WL_DELAY_START_CONNECTION);
        LOG_I("wait wifi init done! %d\r\n", _sta_status);
    }

    int bssid_set_flag = 0;
    char bssid[18] = {0};
    const char *akm = NULL;
    int pmf_cfg = 1;
    uint8_t channel_index = 0;
    uint16_t freq = 0;
    int use_dhcp = 1;

    // freq = phy_channel_to_freq(PHY_BAND_2G4, channel_index);

    wifi_sta_connect((char *)ssid, (char *)password, (bssid_set_flag ? bssid : NULL), (char *)akm, pmf_cfg, freq, freq, use_dhcp);

    return ;
}

void WiFiClass::disconnect()
{
    wifi_sta_disconnect();
}

extern "C" {

}

uint8_t WiFiClass::status()
{
    return _sta_status;
}

WiFiClass WiFi;
