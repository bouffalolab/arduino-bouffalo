#pragma once

/*
 * Wi-Fi STA/AP and TCP/UDP/TLS interfaces follow the shared public API of WiFiS3/WiFiNINA. Support
 * includes WPA2/WPA3/open STA with optional PMF, scan, DHCP, DNS, ping, TCP client/server, UDP
 * unicast/broadcast/multicast, TLS 1.2, static addressing and WPA2/open AP with DHCP.
 * begin/beginAP normally switch interfaces; explicit WIFI_AP_STA retains the other interface. See
 * README for verified mode, hidden-SSID, reconnect and IPv6 paths. Serialize all calls in one
 * task, normally setup/loop; concurrent STA/AP requires one channel.
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
#define WL_NETWORKS_LIST_MAXNUM 10 /* Maximum retained scan results, matching WiFiS3 and WiFiNINA. */

/* Values match WiFiS3. */
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

/* Values match WiFiS3. */
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

enum WiFiMode { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 };

class WiFiClass {
public:
    /**
     * @brief Version number of this library.
     */
    const char *firmwareVersion();

    /**
     * @brief Connect and wait for an address up to setTimeout() (default 10 s). Return
     * WL_CONNECTED, WL_NO_SSID_AVAIL or WL_CONNECT_FAILED; a still-pending connection at
     * timeout returns WL_IDLE_STATUS and continues in the background. Query status()
     * afterward. An existing connection to the same network returns immediately. Passphrases
     * are 8-63 characters; NULL/empty selects an open network.
     */
    int begin(const char *ssid);
    /**
     * @brief Initialize this host fixture interface without claiming physical hardware.
     */
    int begin(const char *ssid, const char *passphrase);
    /**
     * @brief Start an AP and wait up to about 3 s for DHCP, returning WL_AP_LISTENING or
     * WL_AP_FAILED. Disconnect an active/pending STA unless WIFI_AP_STA was selected
     * explicitly. Default channel is 1, supported range 1-13. An 8-63-character passphrase
     * selects WPA2; omitted/empty selects open. config() supplies AP address/netmask and
     * restarts DHCP when changed online; the default is 192.168.4.1/24. The pool contains the
     * next ten addresses in the subnet.
     */
    uint8_t beginAP(const char *ssid);
    /**
     * @brief Start an access point with the selected credentials, channel and association
     * options.
     */
    uint8_t beginAP(const char *ssid, uint8_t channel);
    /**
     * @brief Start an access point with the selected credentials, channel and association
     * options.
     */
    uint8_t beginAP(const char *ssid, const char *passphrase);
    /**
     * @brief Start an access point with the selected credentials, channel and association
     * options.
     */
    uint8_t beginAP(const char *ssid, const char *passphrase, uint8_t channel);
    /**
     * @brief Start an access point with the selected credentials, channel and association
     * options.
     */
    uint8_t beginAP(const char *ssid, const char *passphrase, uint8_t channel, bool hidden, uint8_t maxConnections);
    /**
     * @brief Generic extensions: mode is an interface bit mask; enabling alone does not invent
     * credentials or report an association. Settings live in RAM.
     */
    bool mode(WiFiMode mode);
    /**
     * @brief Return the enabled Wi-Fi interface mode bits.
     */
    WiFiMode getMode() const { return mode_; }
    /**
     * @brief Enable or disable reconnect after an unexpected STA disconnection.
     */
    bool setAutoReconnect(bool enable);
    /**
     * @brief Return the current unexpected-disconnect reconnect policy.
     */
    bool getAutoReconnect();
    /**
     * @brief UTC Unix seconds; 0 before synchronization. Never waits for SNTP.
     */
    unsigned long getTime();
    /**
     * @brief RAM-only DNS label (1..31 bytes). Applies to future DHCP requests; changing it
     * does not disconnect an existing station.
     */
    const char *getHostname();
    /**
     * @brief Validate and set the RAM-resident DHCP hostname for subsequent network use.
     */
    bool setHostname(const char *hostname);
    /**
     * @brief Asynchronous SNTP configuration; default is also started on WiFi init.
     */
    bool configTime(const char *server = "pool.ntp.org");
    /**
     * @brief Enable the supported STA link-local IPv6 address.
     */
    bool enableIpV6();
    /**
     * @brief Return the supported STA link-local IPv6 address as text.
     */
    String localIPv6();
    /**
     * @brief Return STA status independently of an active AP.
     */
    uint8_t stationStatus();
    /**
     * @brief Disconnect STA while leaving AP running, as in WiFiS3. Wait for a pending
     * connection result first. Return WL_DISCONNECTED.
     */
    int disconnect(void);
    /**
     * @brief Disconnect STA, optionally removing its mode bit while preserving AP.
     */
    int disconnect(bool wifiOff);
    /**
     * @brief Stop AP, optionally removing its mode bit while preserving STA.
     */
    bool softAPdisconnect(bool wifiOff = false);
    /**
     * @brief Disconnect STA and stop AP; the initialized Wi-Fi stack is retained.
     */
    void end(void);
    /**
     * @brief AP status is WL_AP_LISTENING or WL_AP_CONNECTED with clients. A failed start
     * reports WL_AP_FAILED until begin, beginAP or end.
     */
    uint8_t status();
    /**
     * @brief Most recent wl80211 connection/disconnection reason (WLAN_FW_*); 12 means SSID
     * not found.
     */
    uint8_t reasonCode();

    /**
     * @brief In AP-only mode, address and SSID queries describe AP. In STA+AP mode, they
     * describe STA.
     */
    uint8_t *macAddress(uint8_t *mac);
    /**
     * @brief Return the mode-selected local address.
     */
    IPAddress localIP();
    /**
     * @brief Return the mode-selected network mask.
     */
    IPAddress subnetMask();
    /**
     * @brief Return the mode-selected gateway address.
     */
    IPAddress gatewayIP();
    /**
     * @brief Return the selected DNS server address.
     */
    IPAddress dnsIP(int n = 0);
    /**
     * @brief Configure static addressing with WiFiS3 defaults: omitted gateway is x.y.z.1,
     * netmask 255.255.255.0; an address-only call also uses that gateway as DNS. Disable DHCP
     * on future connections and apply immediately when connected. Address 0.0.0.0 restores
     * DHCP. Like WiFiS3/WiFiNINA, this also configures AP; AP currently supports /24. Use
     * configSTA/softAPConfig for independent configuration.
     */
    void config(IPAddress local_ip);
    /**
     * @brief Apply WiFiS3-compatible static addressing and defaults; zero address restores
     * DHCP.
     */
    void config(IPAddress local_ip, IPAddress dns_server);
    /**
     * @brief Apply WiFiS3-compatible static addressing and defaults; zero address restores
     * DHCP.
     */
    void config(IPAddress local_ip, IPAddress dns_server, IPAddress gateway);
    /**
     * @brief Apply WiFiS3-compatible static addressing and defaults; zero address restores
     * DHCP.
     */
    void config(IPAddress local_ip, IPAddress dns_server, IPAddress gateway, IPAddress subnet);
    /**
     * @brief Independent interface configuration, used when STA/AP coexist.
     */
    void configSTA(IPAddress local_ip, IPAddress dns_server, IPAddress gateway, IPAddress subnet);
    /**
     * @brief Configure the AP address and supported subnet, restarting DHCP when necessary.
     */
    bool softAPConfig(IPAddress local_ip, IPAddress gateway, IPAddress subnet);
    /**
     * @brief Return the STA address independently of AP mode.
     */
    IPAddress stationIP();
    /**
     * @brief Return the STA subnet mask independently of AP mode.
     */
    IPAddress stationSubnetMask();
    /**
     * @brief Return the STA gateway independently of AP mode.
     */
    IPAddress stationGatewayIP();
    /**
     * @brief Set nonzero DNS servers. Update static settings directly. With DHCP and an active
     * link, retain the current address as static, matching WiFiS3 through its upstream
     * firmware; before connecting, DHCP still supplies DNS.
     */
    void setDNS(IPAddress dns_server1);
    /**
     * @brief Update nonzero DNS entries using the WiFiS3-compatible DHCP/static policy.
     */
    void setDNS(IPAddress dns_server1, IPAddress dns_server2);
    /**
     * @brief Return the selected current or indexed scan-result SSID.
     */
    const char *SSID();
    /**
     * @brief Copy the current or indexed scan-result BSSID into the caller's six-byte buffer.
     */
    uint8_t *BSSID(uint8_t *bssid);
    /**
     * @brief Return current or indexed scan-result signal strength in dBm.
     */
    int32_t RSSI();
    /**
     * @brief Return 0.0.0.0 or an empty string when AP is stopped.
     */
    IPAddress softAPIP();
    /**
     * @brief Return the active AP SSID, or an empty string when stopped.
     */
    const char *softAPSSID();

    /**
     * @brief Scan nearby networks even before begin(), waiting for completion. Retain up to
     * WL_NETWORKS_LIST_MAXNUM entries in descending RSSI order. Return count, or -1 on
     * failure. Results remain valid until the next scan. Out-of-range queries return empty
     * strings or zero. WiFiS3 encryption categories map WPA/WPA2 to WPA2 and WPA2/WPA3 to
     * WPA3; the backend does not distinguish WEP from open networks.
     */
    int8_t scanNetworks();
    /**
     * @brief Return the selected current or indexed scan-result SSID.
     */
    const char *SSID(uint8_t networkItem);
    /**
     * @brief Return current or indexed scan-result signal strength in dBm.
     */
    int32_t RSSI(uint8_t networkItem);
    /**
     * @brief Return the WiFiS3 encryption category for the selected scan result.
     */
    uint8_t encryptionType(uint8_t networkItem);
    /**
     * @brief Copy the current or indexed scan-result BSSID into the caller's six-byte buffer.
     */
    uint8_t *BSSID(uint8_t networkItem, uint8_t *bssid);
    /**
     * @brief Return the selected scan result's channel, or zero for an invalid index.
     */
    uint8_t channel(uint8_t networkItem);

    /**
     * @brief Return 1 on success.
     */
    int hostByName(const char *aHostname, IPAddress &aResult);
    /**
     * @brief Send count ICMP echo requests, each with a 1 s timeout and 1 s interval. Return
     * mean successful round-trip time in milliseconds, or WL_PING_*.
     */
    int ping(IPAddress ip, uint8_t ttl = 128, uint8_t count = 1);
    /**
     * @brief Send bounded ICMP echo probes and return mean successful RTT or a WL_PING error.
     */
    int ping(const String &hostname, uint8_t ttl = 128, uint8_t count = 1);
    /**
     * @brief Send bounded ICMP echo probes and return mean successful RTT or a WL_PING error.
     */
    int ping(const char *host, uint8_t ttl = 128, uint8_t count = 1);

    /**
     * @brief Set the Wi-Fi association/address wait timeout in milliseconds.
     */
    void setTimeout(unsigned long timeout);

private:
    WiFiMode mode_ = WIFI_OFF;
    unsigned long timeout_ = 10000;
    char ssid_[WL_SSID_MAX_LENGTH + 1] = {};
    char pass_[WL_WPA_KEY_MAX_LENGTH + 2] = {};
    char ap_ssid_[WL_SSID_MAX_LENGTH + 1] = {};
    struct wifi_port_ap *aps_ = nullptr; /* Allocated on the first scan. */
    uint8_t ap_count_ = 0;
};

extern WiFiClass WiFi;
