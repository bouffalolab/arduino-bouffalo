/*
WiFiGeneric.h - Bouffalolab Wifi support.

Copyright (c) 2023 BH6BAO <qqwang@bouffalolab.com>
Copyright (c) 2023 Bouffalo Lab Intelligent Technology (Nanjing) Co., Ltd. All rights reserved.

*/

#include "WiFi.h"
#include "WiFiGeneric.h"

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

#define DBG_TAG "WiFi Gen"
#define WIFI_STACK_SIZE  (1536)
#define TASK_PRIORITY_FW (16)

static TaskHandle_t wifi_fw_task;

arduino_event_id_t wifi_event_id = ARDUINO_EVENT_WIFI_READY;

void wifi_event_handler(uint32_t code)
{
    switch (code) {
        case CODE_WIFI_ON_INIT_DONE: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_INIT_DONE\r\n", __func__);
            wifi_mgmr_init(&conf);
            // wifi_event_id = ARDUINO_EVENT_WIFI_READY;
        } break;
        case CODE_WIFI_ON_MGMR_DONE: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_MGMR_DONE\r\n", __func__);
            wifi_event_id = ARDUINO_EVENT_WIFI_READY;
        } break;
        case CODE_WIFI_ON_SCAN_DONE: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_SCAN_DONE\r\n", __func__);
            wifi_mgmr_sta_scanlist();
            wifi_event_id = ARDUINO_EVENT_WIFI_SCAN_DONE;
        } break;
        case CODE_WIFI_ON_CONNECTED: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_CONNECTED\r\n", __func__);
            void mm_sec_keydump();
            mm_sec_keydump();
            wifi_event_id = ARDUINO_EVENT_WIFI_STA_CONNECTED;
        } break;
        case CODE_WIFI_ON_GOT_IP: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_GOT_IP\r\n", __func__);
            LOG_I("[SYS] Memory left is %d Bytes\r\n", kfree_size());
            wifi_event_id = ARDUINO_EVENT_WIFI_STA_GOT_IP;
        } break;
        case CODE_WIFI_ON_DISCONNECT: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_DISCONNECT\r\n", __func__);
            if (wifi_event_id == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
                wifi_event_id = ARDUINO_EVENT_WIFI_STA_DISCONNECTED;
            } else if ((wifi_event_id == WL_IDLE_STATUS) || (wifi_event_id == ARDUINO_EVENT_WIFI_STA_GOT_IP)) {
                wifi_event_id = ARDUINO_EVENT_WIFI_STA_LOST_IP;
            }
        } break;
        case CODE_WIFI_ON_AP_STARTED: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_AP_STARTED\r\n", __func__);
            wifi_event_id = ARDUINO_EVENT_WIFI_AP_START;
        } break;
        case CODE_WIFI_ON_AP_STOPPED: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_AP_STOPPED\r\n", __func__);
            wifi_event_id = ARDUINO_EVENT_WIFI_AP_STOP;
        } break;
        case CODE_WIFI_ON_AP_STA_ADD: {
            LOG_I("[EVENT] [AP] [ADD] %lld\r\n", xTaskGetTickCount());
            wifi_event_id = ARDUINO_EVENT_WIFI_AP_STACONNECTED;
        } break;
        case CODE_WIFI_ON_AP_STA_DEL: {
            LOG_I("[EVENT] [AP] [DEL] %lld\r\n", xTaskGetTickCount());
            wifi_event_id = ARDUINO_EVENT_WIFI_AP_STADISCONNECTED;
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

WiFiGenericClass::WiFiGenericClass()
{
}

int WiFiGenericClass::get_wifi_status()
{
    return wifi_event_id;
}

bool WiFiGenericClass::init_wifi(void)
{
    tcpip_init(NULL, NULL);
    wifi_start_firmware_task();

    return true;
}

int WiFiGenericClass::wifi_channel_to_freq(uint8_t band, int channel)
{
    if ((band == WiFi_BAND_2G4) && (channel >= 1) && (channel <= 14))
    {
        if (channel == 14)
            return 2484;
        else
            return 2407 + channel * 5;
    }
    else if ((band == WiFi_BAND_5G) && (channel >= 1) && (channel <= 177))
    {
        return WiFi_BAND_5G + channel * 5;
    }

    return 0;
}

int32_t WiFiGenericClass::channel(void)
{
    int channel = 0;
    if (wifi_mgmr_sta_channel_get(&channel) != 0){
        LOG_E("Get channel fail!");
        return channel;
    }
    return channel;
}
