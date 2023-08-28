/*
###############################################################################
# Copyright (c) 2019, PulseRain Technology LLC
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License (LGPL) as
# published by the Free Software Foundation, either version 3 of the License,
# or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE.
# See the GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
###############################################################################
*/

#include "FreeRTOS.h"
#include "FreeRTOSConfig.h"
#include "task.h"
#include "Arduino.h"

// #include "bouffalo_sdk.h"
// #include "pins_arduino.h"
// #include "log.h"

// // Declared weak in Arduino.h to allow user redefinitions.
// int atexit(void (* /*func*/ )()) { return 0; }

// Weak empty variant initialization function.
// May be redefined by variant files.
void initVariant() __attribute__((weak));
void initVariant() { }

static TaskHandle_t loop_task_handle;
#define DBG_TAG "MAIN"

#define WIFI_STACK_SIZE  (1536)
#define TASK_PRIORITY_FW (16)

static struct bflb_device_s *uart0;
static TaskHandle_t wifi_fw_task;

#ifdef __cplusplus
extern "C" {
#include "bl_fw_api.h"
#include "wifi_mgmr_ext.h"
#include "wifi_mgmr.h"
#include "bl616_glb.h"
#include "rfparam_adapter.h"

#include "tcpip.h"

#include "mem.h"
#include "shell.h"
#include "log.h"

static wifi_conf_t conf = {
    .country_code = "CN",
};

    extern void shell_init_with_task(struct bflb_device_s *shell);

void wifi_event_handler(uint32_t code)
{
    switch (code) {
        case CODE_WIFI_ON_INIT_DONE: {
            LOG_I("[APP] [EVT] %s, CODE_WIFI_ON_INIT_DONE\r\n", __func__);
            wifi_mgmr_init(&conf);
        } break;
        case CODE_WIFI_ON_MGMR_DONE: {
            LOG_I("[APP] [EVT] %s, CODE_WIFI_ON_MGMR_DONE\r\n", __func__);
        } break;
        case CODE_WIFI_ON_SCAN_DONE: {
            LOG_I("[APP] [EVT] %s, CODE_WIFI_ON_SCAN_DONE\r\n", __func__);
            wifi_mgmr_sta_scanlist();
        } break;
        case CODE_WIFI_ON_CONNECTED: {
            LOG_I("[APP] [EVT] %s, CODE_WIFI_ON_CONNECTED\r\n", __func__);
            void mm_sec_keydump();
            mm_sec_keydump();
        } break;
        case CODE_WIFI_ON_GOT_IP: {
            LOG_I("[APP] [EVT] %s, CODE_WIFI_ON_GOT_IP\r\n", __func__);
            LOG_I("[SYS] Memory left is %d Bytes\r\n", kfree_size());
        } break;
        case CODE_WIFI_ON_DISCONNECT: {
            LOG_I("[APP] [EVT] %s, CODE_WIFI_ON_DISCONNECT\r\n", __func__);
        } break;
        case CODE_WIFI_ON_AP_STARTED: {
            LOG_I("[APP] [EVT] %s, CODE_WIFI_ON_AP_STARTED\r\n", __func__);
        } break;
        case CODE_WIFI_ON_AP_STOPPED: {
            LOG_I("[APP] [EVT] %s, CODE_WIFI_ON_AP_STOPPED\r\n", __func__);
        } break;
        case CODE_WIFI_ON_AP_STA_ADD: {
            LOG_I("[APP] [EVT] [AP] [ADD] %lld\r\n", xTaskGetTickCount());
        } break;
        case CODE_WIFI_ON_AP_STA_DEL: {
            LOG_I("[APP] [EVT] [AP] [DEL] %lld\r\n", xTaskGetTickCount());
        } break;
        default: {
            LOG_I("[APP] [EVT] Unknown code %u \r\n", code);
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
#endif

void loopTask(void *pvParameters){

    for(;;){
        loop();
        // bflb_mtimer_delay_ms(1000);
        // delay(1000);
    }
}

int main(void)
{
    init();


    uart0 = bflb_device_get_by_name("uart0");
    shell_init_with_task(uart0);

    tcpip_init(NULL, NULL);
    wifi_start_firmware_task();

    //+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
    // ARDUINO sketch
    //+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
    setup();

    // for (;;) {
    //     loop();
    //     bflb_mtimer_delay_ms(1000);
    // } // End of while loop

    //+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

    //freertos
    xTaskCreate(loopTask, (char *)"loop_task", 512, NULL, configMAX_PRIORITIES - 1, &loop_task_handle);

    vTaskStartScheduler();

    while (1) {
    }

    return 0;
} // End of main()
