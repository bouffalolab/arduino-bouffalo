#pragma once

/*
 * Wi-Fi station 与 TCP 客户端，接口同 Arduino 官方 WiFi 库（WiFiS3、WiFiNINA）的公共部分。
 * 已实现：STA（WPA2/WPA3 个人与开放网络，PMF 可选）、DHCP、DNS、ping、WiFiClient。
 * 未实现：AP、扫描、静态地址（config）、WiFiServer、WiFiUDP、TLS。
 * 所有调用在同一个任务中进行（通常是 setup/loop）。
 */
#include "Arduino.h"

#include "WiFiClient.h"

#define WIFI_FIRMWARE_LATEST_VERSION "0.1.0"

#define WL_SSID_MAX_LENGTH    32
#define WL_WPA_KEY_MAX_LENGTH 63
#define WL_MAC_ADDR_LENGTH    6
#define WL_IPV4_LENGTH        4

/* 取值同 WiFiS3 */
typedef enum {
    WL_NO_SHIELD = 255,
    WL_NO_MODULE = WL_NO_SHIELD,
    WL_IDLE_STATUS = 0,
    WL_NO_SSID_AVAIL,
    WL_SCAN_COMPLETED,
    WL_CONNECTED,
    WL_CONNECT_FAILED,
    WL_CONNECTION_LOST,
    WL_DISCONNECTED,
    WL_AP_LISTENING,
    WL_AP_CONNECTED,
    WL_AP_FAILED
} wl_status_t;

typedef enum {
    WL_PING_DEST_UNREACHABLE = -1,
    WL_PING_TIMEOUT = -2,
    WL_PING_UNKNOWN_HOST = -3,
    WL_PING_ERROR = -4
} wl_ping_result_t;

class WiFiClass {
public:
    /* 版本号即本库版本 */
    const char *firmwareVersion();

    /*
     * 连接并等待取得地址，最多 setTimeout() 给定的时间（默认 10 s）。返回 WL_CONNECTED，或
     * WL_NO_SSID_AVAIL、WL_CONNECT_FAILED；超时仍在连接时返回 WL_IDLE_STATUS，连接继续在后台进行，
     * 之后用 status() 查询。已连上同一网络时直接返回 WL_CONNECTED。密码 8–63 个字符，NULL 或空串为开放网络。
     */
    int begin(const char *ssid);
    int begin(const char *ssid, const char *passphrase);
    /* 断开；正在连接时先等它出结果。返回 WL_DISCONNECTED */
    int disconnect(void);
    /* 同 disconnect()；Wi-Fi 协议栈初始化后不释放 */
    void end(void);
    uint8_t status();
    /* 最近一次连接失败或断开时 wl80211 的状态码（WLAN_FW_*，12 为找不到 SSID） */
    uint8_t reasonCode();

    uint8_t *macAddress(uint8_t *mac);
    IPAddress localIP();
    IPAddress subnetMask();
    IPAddress gatewayIP();
    IPAddress dnsIP(int n = 0);
    const char *SSID();
    uint8_t *BSSID(uint8_t *bssid);
    int32_t RSSI();

    /* 成功返回 1 */
    int hostByName(const char *aHostname, IPAddress &aResult);
    /* 发 count 次 ICMP echo（每次最多等 1 s，间隔 1 s）；返回成功各次的平均往返毫秒数，或 WL_PING_* */
    int ping(IPAddress ip, uint8_t ttl = 128, uint8_t count = 1);
    int ping(const String &hostname, uint8_t ttl = 128, uint8_t count = 1);
    int ping(const char *host, uint8_t ttl = 128, uint8_t count = 1);

    void setTimeout(unsigned long timeout);

private:
    unsigned long timeout_ = 10000;
    char ssid_[WL_SSID_MAX_LENGTH + 1] = {};
    char pass_[WL_WPA_KEY_MAX_LENGTH + 2] = {};
};

extern WiFiClass WiFi;
