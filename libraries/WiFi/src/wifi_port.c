/*
 * wl80211/wifi_mgmr 封装，接口说明见 wifi_port.h。
 * 状态由 wifi_mgmr 的 EV_WIFI 事件驱动：CONNECTED（4-way 完成）、GOT_IP（DHCP 取得地址，DHCP 15 s 超时由
 * wifi_mgmr 自行断开）、DISCONNECT（连接失败与连上后断开都是它，原因要在事件到达时读状态码）；扫描完成是 SCAN_DONE。
 * 状态码 12（WLAN_FW_SCAN_NO_BSSID_AND_CHANNEL）表示扫描不到该 SSID。
 * AP：AP_STARTED 之后 wifi_mgmr 另起任务设地址、启动 DHCP 服务；AP_STA_ADD、AP_STA_DEL 增减已连上的设备数。
 * 静态地址：连接时不启动 DHCP；wifi_mgmr 在 CONNECTED 事件之后另起任务把 STA 链路 up，之后由调用库的任务设置地址并
 * 置为已连接（地址与上次相同时 lwIP 不报 GOT_IP）。静态地址时 GOT_IP 0 只来自本文件停止 DHCP，不作断开处理。
 * 本文件不含 lwip/sockets.h：它与 wl80211.h 都定义 struct iovec（socket 在 net_port.c）。
 */
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "lwip/dns.h"
#include "lwip/netifapi.h"
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
#define PENDING_DISCONNECT_TIMEOUT_MS 3000
#define AP_POOL_SIZE       10 /* DHCP 地址池大小，同上游 ESP32 固件 */

/* wl80211 lwip.c 的 netif 表（底层包 libwl80211_plat，SDK 头文件未声明）：静态地址要看 STA 链路并停止、启动 DHCP；
   AP 的地址从它读 */
extern struct netif vif2netif[WL80211_VIF_MAX];
#define STA_NETIF (&vif2netif[WL80211_VIF_STA])
#define AP_NETIF  (&vif2netif[WL80211_VIF_AP])

static int init_result = 1; /* 1 未初始化，0 成功，-1 失败 */
static volatile bool init_task_done;

static volatile enum wifi_port_state state = WIFI_PORT_IDLE;
static volatile bool connecting;
static volatile bool got_ip;
static volatile bool user_disconnect;
static volatile bool pending_disconnect; /* 断开之后连接才完成，要补发断开 */
static volatile uint16_t last_status;
static volatile uint16_t last_reason;
static volatile uint32_t disconnect_events;
static volatile uint32_t scan_done_events;
static uint32_t cfg_ip, cfg_mask, cfg_gw, cfg_dns[2]; /* cfg_ip 为 0 时用 DHCP */
static volatile bool static_pending; /* 已关联，等链路 up 后设置静态地址 */
static volatile bool ap_up;            /* AP_STARTED 之后、AP_STOPPED 之前 */
static volatile bool ap_failed;
static volatile uint8_t ap_stations;
static volatile uint32_t ap_stopped_events;

/*
 * EV_WIFI 回调。async_event_loop 在定时器任务中、调度器挂起时运行（同 macsw_bare），这里只更新状态，
 * 不调用会阻塞的接口。
 */
static void wifi_event(async_input_event_t ev, void *priv)
{
    (void)priv;
    switch (ev->code) {
        case CODE_WIFI_ON_GOT_IP:
            if (ev->value && user_disconnect) {
                /* 应用断开时连接还没出结果（等待超时），之后连接才完成：不报告已连接，下一次调用接口时补发断开 */
                pending_disconnect = true;
                connecting = false;
            } else if (ev->value) {
                got_ip = true;
                connecting = false;
                state = WIFI_PORT_CONNECTED;
            } else if (got_ip && cfg_ip == 0) {
                got_ip = false;
                state = WIFI_PORT_LOST;
            }
            break;
        case CODE_WIFI_ON_CONNECTED:
            if (cfg_ip != 0 && user_disconnect) {
                pending_disconnect = true; /* 同 GOT_IP 时的处理 */
                connecting = false;
            } else if (cfg_ip != 0) {
                static_pending = true;
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
            static_pending = false;
            disconnect_events = disconnect_events + 1;
            break;
        case CODE_WIFI_ON_SCAN_DONE:
            scan_done_events = scan_done_events + 1;
            break;
        case CODE_WIFI_ON_AP_STARTED:
            ap_up = true;
            break;
        case CODE_WIFI_ON_AP_STOPPED:
            ap_up = false;
            ap_stations = 0;
            ap_stopped_events = ap_stopped_events + 1;
            break;
        case CODE_WIFI_ON_AP_STA_ADD:
            ap_stations = ap_stations + 1;
            break;
        case CODE_WIFI_ON_AP_STA_DEL:
            if (ap_stations > 0) {
                ap_stations = ap_stations - 1;
            }
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

/* 发起断开并等断开事件，最多 timeout_ms */
static void sta_disconnect_wait(uint32_t timeout_ms)
{
    uint32_t events = disconnect_events;
    wifi_mgmr_sta_disconnect();
    for (uint32_t waited = 0; disconnect_events == events && waited < timeout_ms; waited += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void apply_static_ip(void)
{
    ip4_addr_t ip, mask, gw;
    ip4_addr_set_u32(&ip, cfg_ip);
    ip4_addr_set_u32(&mask, cfg_mask);
    ip4_addr_set_u32(&gw, cfg_gw);
    netifapi_netif_set_addr(STA_NETIF, &ip, &mask, &gw);
    for (int i = 0; i < 2; i++) {
        if (cfg_dns[i] != 0) {
            ip_addr_t dns;
            ip_addr_set_ip4_u32_val(dns, cfg_dns[i]);
            dns_setserver((u8_t)i, &dns);
        }
    }
    netifapi_netif_set_default(STA_NETIF);
}

/* 静态地址在 STA 链路 up 之后设置（在调用库的任务中） */
static void finish_static_ip(void)
{
    if (static_pending && netif_is_link_up(STA_NETIF)) {
        static_pending = false;
        apply_static_ip();
        got_ip = true;
        connecting = false;
        state = WIFI_PORT_CONNECTED;
    }
}

/* 事件回调中不能调用会阻塞的接口，补发的断开在这里做（在调用库的任务中） */
static void finish_pending_disconnect(void)
{
    if (pending_disconnect) {
        pending_disconnect = false;
        sta_disconnect_wait(PENDING_DISCONNECT_TIMEOUT_MS);
    }
}

int wifi_port_connect(const char *ssid, const char *pass)
{
    wifi_mgmr_sta_connect_params_t p;

    finish_pending_disconnect();

    memset(&p, 0, sizeof(p));
    strncpy(p.ssid, ssid, sizeof(p.ssid) - 1);
    if (pass != NULL) {
        strncpy(p.key, pass, sizeof(p.key) - 1);
    }
    p.pmf_cfg = 1; /* PMF 可选；SDK 默认 0 连不上要求 PMF 的 AP（WPA3，reason 31） */
    p.use_dhcp = cfg_ip == 0;

    user_disconnect = false;
    static_pending = false;
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
        finish_static_ip();
    }
}

/* wifi_mgmr_sta_disconnect 只对已关联的连接有效，所以先等进行中的连接出结果 */
void wifi_port_disconnect(uint32_t timeout_ms)
{
    finish_pending_disconnect();
    wait_until(&connecting, false, timeout_ms);
    user_disconnect = true;
    if (wifi_mgmr_sta_state_get()) {
        sta_disconnect_wait(timeout_ms);
    }
    connecting = false;
    got_ip = false;
    state = WIFI_PORT_DISCONNECTED;
}

enum wifi_port_state wifi_port_state(void)
{
    finish_pending_disconnect();
    finish_static_ip();
    if (init_result == -1) {
        return WIFI_PORT_NO_MODULE;
    }
    if (ap_up) {
        return ap_stations > 0 ? WIFI_PORT_AP_CONNECTED : WIFI_PORT_AP_LISTENING;
    }
    return ap_failed ? WIFI_PORT_AP_FAILED : state;
}

bool wifi_port_net_up(void)
{
    enum wifi_port_state s = wifi_port_state();
    return s == WIFI_PORT_CONNECTED || s == WIFI_PORT_AP_LISTENING || s == WIFI_PORT_AP_CONNECTED;
}

bool wifi_port_connecting(void)
{
    finish_static_ip();
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
    if (ap_up) {
        *ip = *gw = ip4_addr_get_u32(netif_ip4_addr(AP_NETIF));
        *mask = ip4_addr_get_u32(netif_ip4_netmask(AP_NETIF));
    } else if (state == WIFI_PORT_CONNECTED) {
        wifi_mgmr_sta_ip_get(ip, mask, gw, &dns);
    }
}

void wifi_port_ip_config(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns1, uint32_t dns2)
{
    bool was_static = cfg_ip != 0;
    cfg_ip = ip;
    cfg_mask = mask;
    cfg_gw = gw;
    cfg_dns[0] = ip != 0 ? dns1 : 0;
    cfg_dns[1] = ip != 0 ? dns2 : 0;
    if (state != WIFI_PORT_CONNECTED) {
        return;
    }
    if (ip != 0) {
        if (!was_static) {
            netifapi_dhcp_stop(STA_NETIF); /* 地址清零后的 GOT_IP 0 在静态地址时不作断开 */
        }
        apply_static_ip();
    } else if (was_static) {
        netifapi_dhcp_start(STA_NETIF);
    }
}

void wifi_port_set_dns(uint32_t dns1, uint32_t dns2)
{
    if (cfg_ip == 0) {
        if (state == WIFI_PORT_CONNECTED) {
            uint32_t ip, mask, gw, dns;
            wifi_mgmr_sta_ip_get(&ip, &mask, &gw, &dns);
            wifi_port_ip_config(ip, mask, gw, dns1, dns2);
        }
        return;
    }
    if (dns1 != 0) {
        cfg_dns[0] = dns1;
    }
    if (dns2 != 0) {
        cfg_dns[1] = dns2;
    }
    if (state == WIFI_PORT_CONNECTED) {
        apply_static_ip();
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
    if (ap_up) {
        memcpy(mac, AP_NETIF->hwaddr, 6);
        return 0;
    }
    return wifi_mgmr_sta_mac_get(mac);
}

int wifi_port_bssid(uint8_t bssid[6])
{
    memset(bssid, 0, 6);
    return state == WIFI_PORT_CONNECTED ? wifi_mgmr_sta_get_bssid(bssid) : -1;
}

/* WiFiS3 对上游各加密方式的归类：WPA/WPA2 混合算 WPA2，WPA2/WPA3 混合算 WPA3。
   wifi_mgmr 不看 privacy 位，WEP 网络会归为开放 */
static uint8_t to_enc(uint8_t auth)
{
    switch (auth) {
        case WIFI_EVENT_BEACON_IND_AUTH_OPEN: return WIFI_PORT_ENC_NONE;
        case WIFI_EVENT_BEACON_IND_AUTH_WEP: return WIFI_PORT_ENC_WEP;
        case WIFI_EVENT_BEACON_IND_AUTH_WPA_PSK: return WIFI_PORT_ENC_WPA;
        case WIFI_EVENT_BEACON_IND_AUTH_WPA2_PSK:
        case WIFI_EVENT_BEACON_IND_AUTH_WPA_WPA2_PSK: return WIFI_PORT_ENC_WPA2;
        case WIFI_EVENT_BEACON_IND_AUTH_WPA_ENT: return WIFI_PORT_ENC_WPA2_ENTERPRISE;
        case WIFI_EVENT_BEACON_IND_AUTH_WPA3_SAE:
        case WIFI_EVENT_BEACON_IND_AUTH_WPA2_PSK_WPA3_SAE: return WIFI_PORT_ENC_WPA3;
        default: return WIFI_PORT_ENC_UNKNOWN;
    }
}

int wifi_port_scan(struct wifi_port_ap *aps, int max, uint32_t timeout_ms)
{
    wifi_mgmr_scan_params_t p;

    if (max <= 0) {
        return -1;
    }
    finish_pending_disconnect();
    wait_until(&connecting, false, timeout_ms);
    wifi_mgmr_sta_scanlist_free(); /* 结果表跨次累积，同 SDK 的 wifi_scan 命令先清空 */
    memset(&p, 0, sizeof(p));
    uint32_t events = scan_done_events;
    if (wifi_mgmr_sta_scan(&p) != 0) {
        return -1;
    }
    for (uint32_t waited = 0; scan_done_events == events; waited += 10) {
        if (waited >= timeout_ms) {
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    wifi_mgmr_scan_item_t *items = malloc(max * sizeof(*items));
    if (items == NULL) {
        return -1;
    }
    int n = (int)wifi_mgmr_sta_scanlist_dump(items, max); /* 按 RSSI 从强到弱 */
    for (int i = 0; i < n; i++) {
        memcpy(aps[i].ssid, items[i].ssid, 32);
        aps[i].ssid[32] = '\0';
        memcpy(aps[i].bssid, items[i].bssid, 6);
        aps[i].rssi = items[i].rssi;
        aps[i].channel = items[i].channel;
        aps[i].enc = to_enc(items[i].auth);
    }
    free(items);
    return n;
}

/* 发起停止并等停止事件；AP 尚未报告启动时不等 */
static void ap_stop_wait(uint32_t timeout_ms)
{
    uint32_t events = ap_stopped_events;
    wifi_mgmr_ap_stop();
    for (uint32_t waited = 0; ap_up && ap_stopped_events == events && waited < timeout_ms; waited += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ap_up = false;
    ap_stations = 0;
}

int wifi_port_ap_start(const char *ssid, const char *pass, uint8_t channel, uint32_t timeout_ms)
{
    wifi_mgmr_ap_params_t p;
    ip4_addr_t ip, zero;

    if (ap_up) {
        ap_stop_wait(timeout_ms);
    }
    ip4_addr_set_u32(&ip, cfg_ip != 0 ? cfg_ip : PP_HTONL(LWIP_MAKEU32(192, 168, 4, 1)));
    uint8_t host = ip4_addr4(&ip);
    memset(&p, 0, sizeof(p));
    p.ssid = (char *)ssid;
    p.key = pass != NULL && pass[0] != '\0' ? (char *)pass : NULL; /* 有密码时 wl80211 用 WPA2-PSK */
    p.channel = channel;
    p.use_ipcfg = true;
    p.use_dhcpd = true;
    /* dhcpd_start 只在给出地址池时保留已设的地址（否则改为 192.168.169.1），池在地址的最后一段前后 */
    p.start = host <= 254 - AP_POOL_SIZE ? host + 1 : host - AP_POOL_SIZE;
    p.limit = AP_POOL_SIZE;
    p.ap_ipaddr = ip4_addr_get_u32(&ip);
    p.ap_mask = cfg_ip != 0 ? cfg_mask : PP_HTONL(LWIP_MAKEU32(255, 255, 255, 0));
    ap_failed = false;
    ap_stations = 0;
    /* 清掉上次的地址，下面才能按地址判断这次的设置已完成 */
    ip4_addr_set_zero(&zero);
    netifapi_netif_set_addr(AP_NETIF, &zero, &zero, &zero);
    if (wifi_mgmr_ap_start(&p) != 0) {
        ap_failed = true;
        return -1;
    }
    for (uint32_t waited = 0; waited < timeout_ms; waited += 10) {
        if (ap_up && netif_is_link_up(AP_NETIF) && ip4_addr_cmp(netif_ip4_addr(AP_NETIF), &ip)) {
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ap_stop_wait(timeout_ms);
    ap_failed = true;
    return -1;
}

void wifi_port_ap_stop(uint32_t timeout_ms)
{
    ap_failed = false;
    if (ap_up) {
        ap_stop_wait(timeout_ms);
    }
}
