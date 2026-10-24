#pragma once

/*
 * Wi-Fi station 与 TCP 客户端，接口同 Arduino 官方 WiFi 库（WiFiS3、WiFiNINA）的公共部分。
 * 已实现：STA（WPA2/WPA3 个人与开放网络，PMF 可选）、扫描、DHCP、DNS、ping、WiFiClient、WiFiServer、WiFiUDP（单播与广播）、
 * WiFiSSLClient（TLS 1.2）。
 * 未实现：AP、静态地址（config）、UDP 组播。
 * 所有调用在同一个任务中进行（通常是 setup/loop）。
 */
#include "Arduino.h"

#include "WiFiClient.h"
#include "WiFiSSLClient.h"
#include "WiFiServer.h"
#include "WiFiUdp.h"

#define WIFI_FIRMWARE_LATEST_VERSION "0.1.0"

#define WL_SSID_MAX_LENGTH    32
#define WL_WPA_KEY_MAX_LENGTH 63
#define WL_MAC_ADDR_LENGTH    6
#define WL_IPV4_LENGTH        4
#define WL_NETWORKS_LIST_MAXNUM 10 /* scanNetworks 保留的网络数，同 WiFiS3、WiFiNINA */

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

/* 取值同 WiFiS3 */
enum wl_enc_type {
    ENC_TYPE_WEP,
    ENC_TYPE_WPA,
    ENC_TYPE_TKIP = ENC_TYPE_WPA,
    ENC_TYPE_WPA2,
    ENC_TYPE_CCMP = ENC_TYPE_WPA2,
    ENC_TYPE_WPA2_ENTERPRISE,
    ENC_TYPE_WPA3,
    ENC_TYPE_NONE,
    ENC_TYPE_AUTO,
    ENC_TYPE_UNKNOWN = 255
};

typedef enum {
    WL_PING_DEST_UNREACHABLE = -1,
    WL_PING_TIMEOUT = -2,
    WL_PING_UNKNOWN_HOST = -3,
    WL_PING_ERROR = -4
} wl_ping_result_t;

struct wifi_port_ap;

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

    /*
     * 扫描附近网络并等待完成（未调用 begin 时也可用），按 RSSI 从强到弱保留前 WL_NETWORKS_LIST_MAXNUM 个，
     * 返回个数，失败返回 -1。结果到下次扫描前有效，用下面带序号的函数读取；序号越界时返回空串或 0。
     * 加密方式按 WiFiS3 归类（WPA/WPA2 混合为 ENC_TYPE_WPA2，WPA2/WPA3 混合为 ENC_TYPE_WPA3）；底层不区分 WEP 与开放网络
     */
    int8_t scanNetworks();
    const char *SSID(uint8_t networkItem);
    int32_t RSSI(uint8_t networkItem);
    uint8_t encryptionType(uint8_t networkItem);
    uint8_t *BSSID(uint8_t networkItem, uint8_t *bssid);
    uint8_t channel(uint8_t networkItem);

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
    struct wifi_port_ap *aps_ = nullptr; /* 第一次扫描时分配 */
    uint8_t ap_count_ = 0;
};

extern WiFiClass WiFi;
