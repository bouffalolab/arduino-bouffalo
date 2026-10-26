#pragma once

/*
 * Wi-Fi station、AP 与 TCP/UDP/TLS，接口同 Arduino 官方 WiFi 库（WiFiS3、WiFiNINA）的公共部分。
 * 已实现：STA（WPA2/WPA3 个人与开放网络，PMF 可选）、扫描、DHCP、DNS、ping、WiFiClient、WiFiServer、WiFiUDP（单播与广播）、
 * WiFiSSLClient（TLS 1.2）、静态地址（config、setDNS）、AP（beginAP：WPA2 或开放网络，带 DHCP 服务）。
 * 同一时间只用 STA 或 AP 之一。AP 运行时 WiFiServer、WiFiUDP 已在实板上验证（电脑连入后 TCP、UDP 收发）；
 * WiFiClient::connect、ping(IPAddress) 在 AP 下未在实板上验证。
 * 未实现：STA 与 AP 同时运行、隐藏 SSID、UDP 组播。
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
    /*
     * 启动 AP 并等 DHCP 服务启动（最多约 3 s），返回 WL_AP_LISTENING 或 WL_AP_FAILED。STA 已连接或正在连接时先断开。
     * 省略的信道为 1（同 WiFiS3），可用 1–13；密码 8–63 个字符时为 WPA2，省略或空串为开放网络。
     * 地址为 config() 给出的地址与掩码（AP 运行时调用 config，下次 beginAP 时生效），未调用时为 192.168.4.1/24；
     * DHCP 地址池为同一网段中紧邻该地址的 10 个
     */
    uint8_t beginAP(const char *ssid);
    uint8_t beginAP(const char *ssid, uint8_t channel);
    uint8_t beginAP(const char *ssid, const char *passphrase);
    uint8_t beginAP(const char *ssid, const char *passphrase, uint8_t channel);
    /* 断开 STA（AP 照常运行，同 WiFiS3）；正在连接时先等它出结果。返回 WL_DISCONNECTED */
    int disconnect(void);
    /* 断开 STA 并停止 AP；Wi-Fi 协议栈初始化后不释放 */
    void end(void);
    /* AP 运行时为 WL_AP_LISTENING，有设备连上时为 WL_AP_CONNECTED；启动 AP 失败后为 WL_AP_FAILED，直到下次 begin、beginAP 或 end */
    uint8_t status();
    /* 最近一次连接失败或断开时 wl80211 的状态码（WLAN_FW_*，12 为找不到 SSID） */
    uint8_t reasonCode();

    /* AP 运行时 macAddress、localIP、subnetMask、gatewayIP、SSID 都是 AP 的（网关即 AP 地址） */
    uint8_t *macAddress(uint8_t *mac);
    IPAddress localIP();
    IPAddress subnetMask();
    IPAddress gatewayIP();
    IPAddress dnsIP(int n = 0);
    /*
     * 静态地址，取值同 WiFiS3：省略的网关为 local_ip 的 x.y.z.1，掩码为 255.255.255.0，只给地址时 DNS 也用该网关。
     * 之后每次连接都不启动 DHCP；已连接时立即生效。local_ip 为 0.0.0.0 时恢复 DHCP（同上游 ESP32 固件）。
     * 地址与掩码也用于之后启动的 AP（同 WiFiS3、WiFiNINA）
     */
    void config(IPAddress local_ip);
    void config(IPAddress local_ip, IPAddress dns_server);
    void config(IPAddress local_ip, IPAddress dns_server, IPAddress gateway);
    void config(IPAddress local_ip, IPAddress dns_server, IPAddress gateway, IPAddress subnet);
    /*
     * 设 DNS 服务器（0.0.0.0 的不改）。静态地址时更新所用的 DNS；用 DHCP 且已连接时以当前地址转为静态地址（同 WiFiS3
     * 经上游固件的结果）；用 DHCP 且未连接时不起作用，DNS 由 DHCP 给出
     */
    void setDNS(IPAddress dns_server1);
    void setDNS(IPAddress dns_server1, IPAddress dns_server2);
    const char *SSID();
    uint8_t *BSSID(uint8_t *bssid);
    int32_t RSSI();
    /* AP 未运行时为 0.0.0.0、空串 */
    IPAddress softAPIP();
    const char *softAPSSID();

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
    char ap_ssid_[WL_SSID_MAX_LENGTH + 1] = {};
    struct wifi_port_ap *aps_ = nullptr; /* 第一次扫描时分配 */
    uint8_t ap_count_ = 0;
};

extern WiFiClass WiFi;
