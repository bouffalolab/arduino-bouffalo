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
    WIFI_PORT_CONNECTED = 3,     /* 已关联且取得地址（DHCP 或静态） */
    WIFI_PORT_FAILED = 4,        /* 连接失败（认证、关联或 DHCP） */
    WIFI_PORT_LOST = 5,          /* 连上后断开 */
    WIFI_PORT_DISCONNECTED = 6,  /* 应用调用了断开 */
    WIFI_PORT_AP_LISTENING = 7,  /* AP 运行中，没有设备连上 */
    WIFI_PORT_AP_CONNECTED = 8,  /* AP 运行中，至少一个设备连上 */
    WIFI_PORT_AP_FAILED = 9,     /* 最近一次启动 AP 失败 */
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
/* 发起连接（PMF 可选，地址按 wifi_port_ip_config），不等待结果；pass 为 NULL 或空串表示开放网络。成功发起返回 0 */
int wifi_port_connect(const char *ssid, const char *pass);
/* 断开并等待断开事件，最多 timeout_ms */
void wifi_port_disconnect(uint32_t timeout_ms);
/* AP 运行时报 AP 的状态，否则报 STA 的 */
enum wifi_port_state wifi_port_state(void);
/* Independent STA state/address, including while AP is running. */
enum wifi_port_state wifi_port_sta_state(void);
void wifi_port_sta_ip(uint32_t *ip, uint32_t *mask, uint32_t *gw);
bool wifi_port_ap_running(void);
void wifi_port_ap_ip(uint32_t *ip, uint32_t *mask, uint32_t *gw);
/* AP address configuration is independent of STA; live changes restart DHCP. */
int wifi_port_ap_config(uint32_t ip, uint32_t mask, uint32_t gw);
int wifi_port_set_auto_reconnect(bool enable);
bool wifi_port_get_auto_reconnect(void);
int wifi_port_enable_ipv6(void);
void wifi_port_ipv6(char *address, uint32_t size);
/* 可以使用 socket：STA 已连接或 AP 运行中 */
bool wifi_port_net_up(void);
/* 正在连接（已发起、尚无结果） */
bool wifi_port_connecting(void);
/* 最近一次连接失败或断开时 wl80211 给出的状态码（WLAN_FW_*）与 802.11 原因码 */
void wifi_port_last_error(uint16_t *status, uint16_t *reason);

/* AP 运行时为 AP 的地址（网关即该地址），STA 已连接时为 STA 的，否则都为 0 */
void wifi_port_ip(uint32_t *ip, uint32_t *mask, uint32_t *gw);
/*
 * 地址配置，之后每次连接都按它进行，已连接时立即生效。ip 为 0 时用 DHCP（DNS 由 DHCP 给出，dns1、dns2 不用）；
 * 否则用静态地址、不启动 DHCP，dns1、dns2 为 0 时不改对应的 DNS 服务器。只配置 STA。
 */
void wifi_port_ip_config(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns1, uint32_t dns2);
/*
 * 设 DNS 服务器，为 0 的不改。静态地址时更新所用的 DNS；用 DHCP 且已连接时以当前地址转为静态地址（同 WiFiS3 的
 * setDNS 经上游固件的结果）；用 DHCP 且未连接时不起作用（DHCP 给出 DNS）
 */
void wifi_port_set_dns(uint32_t dns1, uint32_t dns2);
uint32_t wifi_port_dns(int index);
/* 未连接时返回 0 */
int32_t wifi_port_rssi(void);
/* AP 运行时为 AP 接口的地址 */
int wifi_port_mac(uint8_t mac[6]);
int wifi_port_bssid(uint8_t bssid[6]);

/*
 * 主动扫描全部信道，等扫描完成最多 timeout_ms；正在连接时先等连接出结果（同样最多 timeout_ms）。
 * 按 RSSI 从强到弱把前 max 个网络写入 aps，返回个数；失败或超时返回 -1
 */
int wifi_port_scan(struct wifi_port_ap *aps, int max, uint32_t timeout_ms);

/*
 * 启动 AP（已运行时先停止）并等地址设好、DHCP 服务启动，最多 timeout_ms；不改变 STA。
 * pass 为 NULL 或空串为开放网络，否则 WPA2。地址见 wifi_port_ap_config，DHCP 地址池为同一网段中紧邻该地址的 10 个。
 * 成功返回 0，失败时状态为 WIFI_PORT_AP_FAILED
 */
int wifi_port_ap_start(const char *ssid, const char *pass, uint8_t channel, bool hidden, uint8_t max_connections, uint32_t timeout_ms);
/* 停止 AP 并等停止事件，最多 timeout_ms；清除 WIFI_PORT_AP_FAILED */
void wifi_port_ap_stop(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
