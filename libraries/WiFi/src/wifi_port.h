#pragma once

/*
 * wl80211/wifi_mgmr 的薄封装，供 WiFiClass 使用；实现在 wifi_port.c（SDK 头不进入 C++ 编译单元）。
 * 地址均为网络字节序。
 */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 与 WiFi.h 的 wl_status_t 取值相同 */
enum wifi_port_state {
    WIFI_PORT_IDLE = 0,          /* 未连接过，或正在连接 */
    WIFI_PORT_NO_SSID = 1,       /* 扫描不到该 SSID */
    WIFI_PORT_CONNECTED = 3,     /* 已关联且 DHCP 取得地址 */
    WIFI_PORT_FAILED = 4,        /* 连接失败（认证、关联或 DHCP） */
    WIFI_PORT_LOST = 5,          /* 连上后断开 */
    WIFI_PORT_DISCONNECTED = 6,  /* 应用调用了断开 */
    WIFI_PORT_NO_MODULE = 255,   /* Wi-Fi 初始化失败 */
};

/* 首次调用时初始化 Wi-Fi 与网络栈（顺序同 SDK examples/wifi/macsw_bare）；成功返回 0，之后再调用直接返回 */
int wifi_port_init(void);
/* 发起连接（PMF 可选、DHCP），不等待结果；pass 为 NULL 或空串表示开放网络。成功发起返回 0 */
int wifi_port_connect(const char *ssid, const char *pass);
/* 断开并等待断开事件，最多 timeout_ms */
void wifi_port_disconnect(uint32_t timeout_ms);
enum wifi_port_state wifi_port_state(void);
/* 正在连接（已发起、尚无结果） */
bool wifi_port_connecting(void);
/* 最近一次连接失败或断开时 wl80211 给出的状态码（WLAN_FW_*）与 802.11 原因码 */
void wifi_port_last_error(uint16_t *status, uint16_t *reason);

void wifi_port_ip(uint32_t *ip, uint32_t *mask, uint32_t *gw);
uint32_t wifi_port_dns(int index);
/* 未连接时返回 0 */
int32_t wifi_port_rssi(void);
int wifi_port_mac(uint8_t mac[6]);
int wifi_port_bssid(uint8_t bssid[6]);

#ifdef __cplusplus
}
#endif
