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

/* 与 WiFi.h 的 wl_enc_type 取值相同 */
enum wifi_port_enc {
    WIFI_PORT_ENC_WEP = 0,
    WIFI_PORT_ENC_WPA = 1,
    WIFI_PORT_ENC_WPA2 = 2,            /* 含 WPA/WPA2 混合 */
    WIFI_PORT_ENC_WPA2_ENTERPRISE = 3,
    WIFI_PORT_ENC_WPA3 = 4,            /* 含 WPA2/WPA3 混合 */
    WIFI_PORT_ENC_NONE = 5,
    WIFI_PORT_ENC_UNKNOWN = 255,
};

/* 扫描到的一个网络；隐藏网络的 ssid 为空串 */
struct wifi_port_ap {
    char ssid[33];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    uint8_t enc; /* enum wifi_port_enc */
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

/*
 * 主动扫描全部信道，等扫描完成最多 timeout_ms；正在连接时先等连接出结果（同样最多 timeout_ms）。
 * 按 RSSI 从强到弱把前 max 个网络写入 aps，返回个数；失败或超时返回 -1
 */
int wifi_port_scan(struct wifi_port_ap *aps, int max, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
