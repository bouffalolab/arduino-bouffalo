/*
 * wl80211/wifi_mgmr 封装，接口说明见 wifi_port.h。
 * 状态由 wifi_mgmr 的 EV_WIFI 事件驱动：CONNECTED（4-way 完成）、GOT_IP（DHCP 取得地址，DHCP 15 s 超时由
 * wifi_mgmr 自行断开）、DISCONNECT（连接失败与连上后断开都是它，原因要在事件到达时读状态码）。
 * 状态码 12（WLAN_FW_SCAN_NO_BSSID_AND_CHANNEL）表示扫描不到该 SSID。
 * 本文件不含 lwip/sockets.h：它与 wl80211.h 都定义 struct iovec（socket 在 net_port.c）。
 */
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include "wl80211.h"
#include "wifi_mgmr.h"
#include "macsw_plat.h"
#include "rfparam_adapter.h"
#include "async_event.h"
#include "board_overlay.h"

#include "wifi_port.h"

#define STATUS_NO_SSID     12
#define INIT_TIMEOUT_MS    5000
#define INIT_TASK_STACK    1024 /* 字 */
#define INIT_TASK_PRIORITY 10

static int init_result = 1; /* 1 未初始化，0 成功，-1 失败 */
static volatile bool init_task_done;

static volatile enum wifi_port_state state = WIFI_PORT_IDLE;
static volatile bool connecting;
static volatile bool got_ip;
static volatile bool user_disconnect;
static volatile uint16_t last_status;
static volatile uint16_t last_reason;
static volatile uint32_t disconnect_events;

/*
 * EV_WIFI 回调。async_event_loop 在定时器任务中、调度器挂起时运行（同 macsw_bare），这里只更新状态，
 * 不调用会阻塞的接口。
 */
static void wifi_event(async_input_event_t ev, void *priv)
{
    (void)priv;
    switch (ev->code) {
        case CODE_WIFI_ON_GOT_IP:
            if (ev->value) {
                got_ip = true;
                connecting = false;
                state = WIFI_PORT_CONNECTED;
            } else if (got_ip) {
                got_ip = false;
                state = WIFI_PORT_LOST;
            }
            break;
        case CODE_WIFI_ON_DISCONNECT:
            last_status = wifi_mgmr_sta_info_status_code_get();
            last_reason = wifi_mgmr_sta_info_reason_code_get();
            if (user_disconnect) {
                state = WIFI_PORT_DISCONNECTED;
            } else if (got_ip || state == WIFI_PORT_CONNECTED) {
                state = WIFI_PORT_LOST;
            } else if (connecting) {
                state = last_status == STATUS_NO_SSID ? WIFI_PORT_NO_SSID : WIFI_PORT_FAILED;
            }
            connecting = false;
            got_ip = false;
            disconnect_events = disconnect_events + 1;
            break;
        default:
            break;
    }
}

static void async_event_handler(void *arg1, uint32_t arg2)
{
    (void)arg1;
    (void)arg2;
    vTaskSuspendAll();
    async_event_loop();
    xTaskResumeAll();
}

static void async_event_loop_wake(void)
{
    TickType_t wait = portMAX_DELAY;

    if (xTimerGetTimerDaemonTaskHandle() == xTaskGetCurrentTaskHandle()) {
        wait = 0;
    }
    xTimerPendFunctionCall(async_event_handler, NULL, 0, wait);
}

static void wifi_init_task(void *arg)
{
    (void)arg;
    wifi_task_create();
    tcpip_init(NULL, NULL);
    wl80211_init();
    wifi_mgmr_init();
    async_register_event_filter(EV_WIFI, wifi_event, NULL);
    init_task_done = true;
    vTaskDelete(NULL);
}

int wifi_port_init(void)
{
    if (init_result != 1) {
        return init_result;
    }
    init_result = -1;
    board_wifi_irq_init();
    if (rfparam_init(0, NULL, 0) != 0) {
        return -1;
    }
    async_event_init(async_event_loop_wake);
    if (xTaskCreate(wifi_init_task, "wifi init", INIT_TASK_STACK, NULL, INIT_TASK_PRIORITY, NULL) != pdPASS) {
        return -1;
    }
    for (uint32_t waited = 0; !init_task_done && waited < INIT_TIMEOUT_MS; waited += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (init_task_done) {
        init_result = 0;
    }
    return init_result;
}

int wifi_port_connect(const char *ssid, const char *pass)
{
    wifi_mgmr_sta_connect_params_t p;

    memset(&p, 0, sizeof(p));
    strncpy(p.ssid, ssid, sizeof(p.ssid) - 1);
    if (pass != NULL) {
        strncpy(p.key, pass, sizeof(p.key) - 1);
    }
    p.pmf_cfg = 1; /* PMF 可选；SDK 默认 0 连不上要求 PMF 的 AP（WPA3，reason 31） */
    p.use_dhcp = 1;

    user_disconnect = false;
    got_ip = false;
    state = WIFI_PORT_IDLE;
    connecting = true;
    int ret = wifi_mgmr_sta_connect(&p);
    if (ret != 0) {
        connecting = false;
        state = WIFI_PORT_FAILED;
    }
    return ret;
}

static void wait_until(volatile bool *flag, bool value, uint32_t timeout_ms)
{
    for (uint32_t waited = 0; *flag != value && waited < timeout_ms; waited += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* wifi_mgmr_sta_disconnect 只对已关联的连接有效，所以先等进行中的连接出结果 */
void wifi_port_disconnect(uint32_t timeout_ms)
{
    wait_until(&connecting, false, timeout_ms);
    user_disconnect = true;
    if (wifi_mgmr_sta_state_get()) {
        uint32_t events = disconnect_events;
        wifi_mgmr_sta_disconnect();
        for (uint32_t waited = 0; disconnect_events == events && waited < timeout_ms; waited += 10) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    connecting = false;
    got_ip = false;
    state = WIFI_PORT_DISCONNECTED;
}

enum wifi_port_state wifi_port_state(void)
{
    return init_result == -1 ? WIFI_PORT_NO_MODULE : state;
}

bool wifi_port_connecting(void)
{
    return connecting;
}

void wifi_port_last_error(uint16_t *status, uint16_t *reason)
{
    *status = last_status;
    *reason = last_reason;
}

/* 断开事件到达时 netif 可能还留着旧地址，所以只在已连接时读 */
void wifi_port_ip(uint32_t *ip, uint32_t *mask, uint32_t *gw)
{
    uint32_t dns;

    *ip = *mask = *gw = 0;
    if (state == WIFI_PORT_CONNECTED) {
        wifi_mgmr_sta_ip_get(ip, mask, gw, &dns);
    }
}

uint32_t wifi_port_dns(int index)
{
    if (state != WIFI_PORT_CONNECTED || index < 0 || index >= DNS_MAX_SERVERS) {
        return 0;
    }
    const ip_addr_t *dns = dns_getserver((u8_t)index);
    return dns != NULL ? ip4_addr_get_u32(ip_2_ip4(dns)) : 0;
}

int32_t wifi_port_rssi(void)
{
    int rssi = 0;

    if (state != WIFI_PORT_CONNECTED || wifi_mgmr_sta_rssi_get(&rssi) != 0) {
        return 0;
    }
    return rssi;
}

int wifi_port_mac(uint8_t mac[6])
{
    return wifi_mgmr_sta_mac_get(mac);
}

int wifi_port_bssid(uint8_t bssid[6])
{
    memset(bssid, 0, 6);
    return state == WIFI_PORT_CONNECTED ? wifi_mgmr_sta_get_bssid(bssid) : -1;
}
