/*
WiFiGeneric.cpp - Bouffalolab Wifi support.

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

arduino_event_id_t wifi_event_id = ARDUINO_EVENT_MAX;

static xQueueHandle _arduino_event_queue;
static TaskHandle_t _arduino_event_task_handle = NULL;
static EventGroupHandle_t _arduino_event_group = NULL;

int postArduinoEvent(arduino_event_t *data)
{
	if(data == NULL){
        return -1;
	}
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	arduino_event_t * event = (arduino_event_t*)malloc(sizeof(arduino_event_t));
	if(event == NULL){
        LOG_E("Arduino Event Malloc Failed!");
        return -1;
	}
	memcpy(event, data, sizeof(arduino_event_t));

    // if (xQueueSend(_arduino_event_queue, &event, portMAX_DELAY) != pdPASS) {
    //     LOG_E("Arduino Event Send Failed!");
    //     return -1;
    // }
    if (xQueueSendFromISR(_arduino_event_queue, &event, &xHigherPriorityTaskWoken) != pdTRUE) {
        LOG_E("Arduino Event Send Failed!");
        return -1;
    }
    if( xHigherPriorityTaskWoken )
    {
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
    return 0;
}

void wifi_event_handler(uint32_t code)
{
    arduino_event_t arduino_event;
	arduino_event.event_id = ARDUINO_EVENT_MAX;

    switch (code) {
        case CODE_WIFI_ON_INIT_DONE: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_INIT_DONE\r\n", __func__);
            wifi_mgmr_init(&conf);
            // wifi_event_id = ARDUINO_EVENT_WIFI_READY;
        } break;
        case CODE_WIFI_ON_MGMR_DONE: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_MGMR_DONE\r\n", __func__);
            wifi_event_id = ARDUINO_EVENT_WIFI_READY;
            arduino_event.event_id = ARDUINO_EVENT_WIFI_READY;
        } break;
        case CODE_WIFI_ON_SCAN_DONE: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_SCAN_DONE\r\n", __func__);
            // wifi_mgmr_sta_scanlist();
            wifi_event_id = ARDUINO_EVENT_WIFI_SCAN_DONE;
            arduino_event.event_id = ARDUINO_EVENT_WIFI_SCAN_DONE;
        } break;
        case CODE_WIFI_ON_CONNECTED: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_CONNECTED\r\n", __func__);
            // void mm_sec_keydump();
            // mm_sec_keydump();
            wifi_event_id = ARDUINO_EVENT_WIFI_STA_CONNECTED;
            arduino_event.event_id = ARDUINO_EVENT_WIFI_STA_CONNECTED;
        } break;
        case CODE_WIFI_ON_GOT_IP: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_GOT_IP\r\n", __func__);
            LOG_I("[SYS] Memory left is %d Bytes\r\n", kfree_size());
            wifi_event_id = ARDUINO_EVENT_WIFI_STA_GOT_IP;
            arduino_event.event_id = ARDUINO_EVENT_WIFI_STA_GOT_IP;
        } break;
        case CODE_WIFI_ON_DISCONNECT: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_DISCONNECT\r\n", __func__);
            if (wifi_event_id == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
                wifi_event_id = ARDUINO_EVENT_WIFI_STA_DISCONNECTED;
                arduino_event.event_id = ARDUINO_EVENT_WIFI_STA_DISCONNECTED;
            } else if ((wifi_event_id == WL_IDLE_STATUS) || (wifi_event_id == ARDUINO_EVENT_WIFI_STA_GOT_IP)) {
                wifi_event_id = ARDUINO_EVENT_WIFI_STA_LOST_IP;
                arduino_event.event_id = ARDUINO_EVENT_WIFI_STA_LOST_IP;
            }
        } break;
        case CODE_WIFI_ON_AP_STARTED: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_AP_STARTED\r\n", __func__);
            wifi_event_id = ARDUINO_EVENT_WIFI_AP_START;
            arduino_event.event_id = ARDUINO_EVENT_WIFI_AP_START;
        } break;
        case CODE_WIFI_ON_AP_STOPPED: {
            LOG_I("[EVENT] %s, CODE_WIFI_ON_AP_STOPPED\r\n", __func__);
            wifi_event_id = ARDUINO_EVENT_WIFI_AP_STOP;
            arduino_event.event_id = ARDUINO_EVENT_WIFI_AP_STOP;
        } break;
        case CODE_WIFI_ON_AP_STA_ADD: {
            LOG_I("[EVENT] [AP] [ADD] %lld\r\n", xTaskGetTickCount());
            wifi_event_id = ARDUINO_EVENT_WIFI_AP_STACONNECTED;
            arduino_event.event_id =ARDUINO_EVENT_WIFI_AP_STACONNECTED;
        } break;
        case CODE_WIFI_ON_AP_STA_DEL: {
            LOG_I("[EVENT] [AP] [DEL] %lld\r\n", xTaskGetTickCount());
            wifi_event_id = ARDUINO_EVENT_WIFI_AP_STADISCONNECTED;
            arduino_event.event_id = ARDUINO_EVENT_WIFI_AP_STADISCONNECTED;
        } break;
        default: {
            LOG_I("[EVENT] Unknown code %u \r\n", code);
        }
    }

    if(arduino_event.event_id < ARDUINO_EVENT_MAX){
        postArduinoEvent(&arduino_event);
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
        return -1;
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
    // printf("wifi_event_id:%d\r\n", wifi_event_id);
    return wifi_event_id;
}

static void _arduino_event_cb(void)
{

}

int WiFiGenericClass::_eventCallback(arduino_event_t *event)
{
    static bool first_connect = true;

    if(!event) return 0;

    LOG_D("Arduino Event: %d - %s", event->event_id, WiFi.eventName(event->event_id));

    if(event->event_id == ARDUINO_EVENT_WIFI_SCAN_DONE) {
        WiFiScanClass::_scanDone();
    }

    return 0;
}

static void _arduino_event_task(void *arg) {
    arduino_event_t *data = NULL;

    for (;;) {
        if(xQueueReceive(_arduino_event_queue, &data, portMAX_DELAY) == pdTRUE){
            WiFiGenericClass::_eventCallback(data);
            free(data);
            data = NULL;
        }
    }
    vTaskDelete(NULL);
    _arduino_event_task_handle = NULL;
}

static bool _start_network_event_task()
{
    if (!_arduino_event_group) {
        _arduino_event_group = xEventGroupCreate();
        if (!_arduino_event_group) {
            LOG_E("Network Event Group Create Failed!");
            return false;
        }
        xEventGroupSetBits(_arduino_event_group, WIFI_DNS_IDLE_BIT);
    }
    if(!_arduino_event_queue){
    	_arduino_event_queue = xQueueCreate(32, sizeof(arduino_event_t*));
        if(!_arduino_event_queue){
            LOG_E("Network Event Queue Create Failed!");
            return false;
        }
    }

    if(!_arduino_event_task_handle){
        xTaskCreate(_arduino_event_task, "arduino_events", 4096, NULL, configMAX_PRIORITIES - 6, &_arduino_event_task_handle);
        if(!_arduino_event_task_handle){
            LOG_E("Network Event Task Start Failed!");
            return false;
        }
    }
    return true;
}

static bool initialized = false;
void WiFiGenericClass::tcpip_init_done(void * arg)
{
    LOG_I("tcpip init done!\r\n");
    initialized = _start_network_event_task();
}

bool WiFiGenericClass::init_wifi(void)
{
    tcpip_init((tcpip_init_done_fn)&tcpip_init_done, NULL);
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

const char * WiFiGenericClass::eventName(arduino_event_id_t id) {
    switch(id) {
        case ARDUINO_EVENT_WIFI_READY: return "WIFI_READY";
        case ARDUINO_EVENT_WIFI_SCAN_DONE: return "SCAN_DONE";
        case ARDUINO_EVENT_WIFI_STA_START: return "STA_START";
        case ARDUINO_EVENT_WIFI_STA_STOP: return "STA_STOP";
        case ARDUINO_EVENT_WIFI_STA_CONNECTED: return "STA_CONNECTED";
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: return "STA_DISCONNECTED";
        case ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE: return "STA_AUTHMODE_CHANGE";
        case ARDUINO_EVENT_WIFI_STA_GOT_IP: return "STA_GOT_IP";
        case ARDUINO_EVENT_WIFI_STA_GOT_IP6: return "STA_GOT_IP6";
        case ARDUINO_EVENT_WIFI_STA_LOST_IP: return "STA_LOST_IP";
        case ARDUINO_EVENT_WIFI_AP_START: return "AP_START";
        case ARDUINO_EVENT_WIFI_AP_STOP: return "AP_STOP";
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED: return "AP_STACONNECTED";
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: return "AP_STADISCONNECTED";
        case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED: return "AP_STAIPASSIGNED";
        case ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED: return "AP_PROBEREQRECVED";
        case ARDUINO_EVENT_WIFI_AP_GOT_IP6: return "AP_GOT_IP6";
        case ARDUINO_EVENT_WIFI_FTM_REPORT: return "FTM_REPORT";
        case ARDUINO_EVENT_ETH_START: return "ETH_START";
        case ARDUINO_EVENT_ETH_STOP: return "ETH_STOP";
        case ARDUINO_EVENT_ETH_CONNECTED: return "ETH_CONNECTED";
        case ARDUINO_EVENT_ETH_DISCONNECTED: return "ETH_DISCONNECTED";
        case ARDUINO_EVENT_ETH_GOT_IP: return "ETH_GOT_IP";
        case ARDUINO_EVENT_ETH_GOT_IP6: return "ETH_GOT_IP6";
        case ARDUINO_EVENT_WPS_ER_SUCCESS: return "WPS_ER_SUCCESS";
        case ARDUINO_EVENT_WPS_ER_FAILED: return "WPS_ER_FAILED";
        case ARDUINO_EVENT_WPS_ER_TIMEOUT: return "WPS_ER_TIMEOUT";
        case ARDUINO_EVENT_WPS_ER_PIN: return "WPS_ER_PIN";
        case ARDUINO_EVENT_WPS_ER_PBC_OVERLAP: return "WPS_ER_PBC_OVERLAP";
        case ARDUINO_EVENT_SC_SCAN_DONE: return "SC_SCAN_DONE";
        case ARDUINO_EVENT_SC_FOUND_CHANNEL: return "SC_FOUND_CHANNEL";
        case ARDUINO_EVENT_SC_GOT_SSID_PSWD: return "SC_GOT_SSID_PSWD";
        case ARDUINO_EVENT_SC_SEND_ACK_DONE: return "SC_SEND_ACK_DONE";
        case ARDUINO_EVENT_PROV_INIT: return "PROV_INIT";
        case ARDUINO_EVENT_PROV_DEINIT: return "PROV_DEINIT";
        case ARDUINO_EVENT_PROV_START: return "PROV_START";
        case ARDUINO_EVENT_PROV_END: return "PROV_END";
        case ARDUINO_EVENT_PROV_CRED_RECV: return "PROV_CRED_RECV";
        case ARDUINO_EVENT_PROV_CRED_FAIL: return "PROV_CRED_FAIL";
        case ARDUINO_EVENT_PROV_CRED_SUCCESS: return "PROV_CRED_SUCCESS";
        default: return "";
    }
}

int WiFiGenericClass::setStatusBits(int bits){
    if(!_arduino_event_group){
        return 0;
    }
    return xEventGroupSetBits(_arduino_event_group, bits);
}

int WiFiGenericClass::clearStatusBits(int bits){
    if(!_arduino_event_group){
        return 0;
    }
    return xEventGroupClearBits(_arduino_event_group, bits);
}
