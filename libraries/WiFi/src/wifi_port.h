#pragma once

/*
 * Thin wl80211/wifi_mgmr adapter for WiFiClass, implemented in wifi_port.c to keep SDK headers out
 * of C++ units. Addresses use network byte order.
 */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Values match WiFi.h wl_status_t. */
enum wifi_port_state {
    WIFI_PORT_IDLE = 0,          /* Never connected or connecting. */
    WIFI_PORT_NO_SSID = 1,       /* SSID not found by scan. */
    WIFI_PORT_CONNECTED = 3,     /* Associated with a DHCP or static address. */
    WIFI_PORT_FAILED = 4,        /* Authentication, association or DHCP failed. */
    WIFI_PORT_LOST = 5,          /* Disconnected after a successful connection. */
    WIFI_PORT_DISCONNECTED = 6,  /* Disconnected by the application. */
    WIFI_PORT_AP_LISTENING = 7,  /* AP running without clients. */
    WIFI_PORT_AP_CONNECTED = 8,  /* AP running with at least one client. */
    WIFI_PORT_AP_FAILED = 9,     /* Most recent AP startup failed. */
    WIFI_PORT_NO_MODULE = 255,   /* Wi-Fi initialization failed. */
};

/* Values match WiFi.h wl_enc_type. */
enum wifi_port_enc {
    WIFI_PORT_ENC_WEP = 0,
    WIFI_PORT_ENC_WPA = 1,
    WIFI_PORT_ENC_WPA2 = 2,            /* Includes mixed WPA/WPA2. */
    WIFI_PORT_ENC_WPA2_ENTERPRISE = 3,
    WIFI_PORT_ENC_WPA3 = 4,            /* Includes mixed WPA2/WPA3. */
    WIFI_PORT_ENC_NONE = 5,
    WIFI_PORT_ENC_UNKNOWN = 255,
};

/* One scan result; hidden networks have an empty ssid. */
struct wifi_port_ap {
    char ssid[33];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    uint8_t enc; /* enum wifi_port_enc */
};

/**
 * @brief Lazily initialize Wi-Fi/networking in the SDK macsw_bare order. Return zero on
 * success; subsequent calls return immediately.
 */
int wifi_port_init(void);
/**
 * @brief Start association with optional PMF and wifi_port_ip_config addresses, without
 * waiting. NULL/empty pass selects open. Return zero if started.
 */
int wifi_port_connect(const char *ssid, const char *pass);
/**
 * @brief Disconnect and wait for the event within timeout_ms.
 */
void wifi_port_disconnect(uint32_t timeout_ms);
/**
 * @brief Report AP status while AP runs, otherwise STA status.
 */
enum wifi_port_state wifi_port_state(void);
/**
 * @brief Independent STA state/address, including while AP is running.
 */
enum wifi_port_state wifi_port_sta_state(void);
/**
 * @brief Copy STA IPv4 address, mask and gateway in network byte order.
 */
void wifi_port_sta_ip(uint32_t *ip, uint32_t *mask, uint32_t *gw);
/**
 * @brief Report whether the AP interface is active.
 */
bool wifi_port_ap_running(void);
/**
 * @brief Copy AP IPv4 address, mask and gateway in network byte order.
 */
void wifi_port_ap_ip(uint32_t *ip, uint32_t *mask, uint32_t *gw);
/**
 * @brief AP address configuration is independent of STA; live changes restart DHCP.
 */
int wifi_port_ap_config(uint32_t ip, uint32_t mask, uint32_t gw);
/**
 * @brief Set reconnect policy for unexpected STA disconnections.
 */
int wifi_port_set_auto_reconnect(bool enable);
/**
 * @brief Return the current unexpected-disconnect reconnect policy.
 */
bool wifi_port_get_auto_reconnect(void);
/**
 * @brief Return UTC epoch seconds, or zero before synchronization.
 */
unsigned long wifi_port_time(void);
/**
 * @brief Return the current RAM-resident hostname.
 */
const char *wifi_port_hostname(void);
/**
 * @brief Validate and update the DHCP hostname; return zero on success.
 */
int wifi_port_set_hostname(const char *hostname);
/**
 * @brief Select the SNTP server and schedule synchronization when networking is ready.
 */
int wifi_port_config_time(const char *server);
/**
 * @brief Enable supported STA link-local IPv6 addressing.
 */
int wifi_port_enable_ipv6(void);
/**
 * @brief Write the STA link-local IPv6 address as bounded text.
 */
void wifi_port_ipv6(char *address, uint32_t size);
/**
 * @brief Sockets may be used when STA is connected or AP is running.
 */
bool wifi_port_net_up(void);
/**
 * @brief Association has started and its result is pending.
 */
bool wifi_port_connecting(void);
/**
 * @brief Latest wl80211 WLAN_FW_* status and 802.11 reason captured on connection failure or
 * disconnect.
 */
void wifi_port_last_error(uint16_t *status, uint16_t *reason);

/**
 * @brief Use AP addresses while AP runs (gateway equals its address), STA addresses while
 * connected, otherwise zeros.
 */
void wifi_port_ip(uint32_t *ip, uint32_t *mask, uint32_t *gw);
/**
 * @brief Configure STA addresses for future connections and apply immediately if connected. ip
 * zero enables DHCP, which supplies DNS and ignores dns1/dns2. Otherwise use static addressing
 * without DHCP; zero DNS entries leave the corresponding servers unchanged.
 */
void wifi_port_ip_config(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns1, uint32_t dns2);
/**
 * @brief Set nonzero DNS servers. Update static settings directly; with active DHCP retain the
 * current address as static, matching WiFiS3 through upstream firmware. Before connection,
 * DHCP still supplies DNS.
 */
void wifi_port_set_dns(uint32_t dns1, uint32_t dns2);
/**
 * @brief Return a DNS server in network byte order, or zero for an invalid index.
 */
uint32_t wifi_port_dns(int index);
/**
 * @brief Return zero while disconnected.
 */
int32_t wifi_port_rssi(void);
/**
 * @brief Use the AP interface address while AP runs.
 */
int wifi_port_mac(uint8_t mac[6]);
/**
 * @brief Copy the connected access point BSSID, reporting failure when unavailable.
 */
int wifi_port_bssid(uint8_t bssid[6]);

/**
 * @brief Actively scan all channels and wait up to timeout_ms. First wait up to timeout_ms for
 * a pending association to finish. Write at most max results into aps in descending RSSI
 * order; return count, or -1 on failure/timeout.
 */
int wifi_port_scan(struct wifi_port_ap *aps, int max, uint32_t timeout_ms);

/**
 * @brief Start AP, stopping an existing AP first, without changing STA. Wait within timeout_ms
 * for addresses and DHCP. NULL/empty pass selects open, otherwise WPA2. wifi_port_ap_config
 * selects addressing; the pool has the next ten addresses in the subnet. Return zero on
 * success; failures report WIFI_PORT_AP_FAILED.
 */
int wifi_port_ap_start(const char *ssid, const char *pass, uint8_t channel, bool hidden, uint8_t max_connections, uint32_t timeout_ms);
/**
 * @brief Stop AP and wait for its event within timeout_ms; clear WIFI_PORT_AP_FAILED.
 */
void wifi_port_ap_stop(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
