/*
 * wl80211/wifi_mgmr adapter; see wifi_port.h. EV_WIFI events drive CONNECTED after four-way
 * authentication, GOT_IP after DHCP (wifi_mgmr disconnects on its 15 s DHCP timeout), and
 * DISCONNECT on either failure or later loss; capture the reason when the event arrives. SCAN_DONE
 * completes scans. AP_STARTED precedes a task that sets addresses/starts DHCP; AP_STA_ADD/DEL
 * maintain client count. With static IP, wait for a task to bring the STA link up after CONNECTED,
 * then set addresses and connected state in the calling task; unchanged addresses need not emit
 * GOT_IP. A static-mode GOT_IP zero from stopping DHCP is not a disconnect. Keep sockets.h
 * separate because it conflicts with wl80211.h over struct iovec.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "lwip/dns.h"
#include "lwip/netifapi.h"
#include "lwip/tcpip.h"
#include "lwip/ip6_addr.h"
#include "lwip/apps/sntp.h"
#include "wl80211.h"
#include "wifi_mgmr.h"
#include "macsw_plat.h"
#include "async_event.h"
#include "board_overlay.h"

#include "wifi_port.h"

#define STATUS_NO_SSID     12
#define INIT_TIMEOUT_MS    5000
#define INIT_TASK_STACK    1024 /* Words. */
#define INIT_TASK_PRIORITY 10
#define PENDING_DISCONNECT_TIMEOUT_MS 3000
#define AP_POOL_SIZE       10 /* DHCP pool size, matching the upstream ESP32 firmware. */

/*
 * wl80211 lwip.c netif table from libwl80211_plat; not declared in exported SDK headers. Static
 * configuration needs STA link/DHCP control; AP addresses are read from this table.
 */
extern struct netif vif2netif[WL80211_VIF_MAX];
#define STA_NETIF (&vif2netif[WL80211_VIF_STA])
#define AP_NETIF  (&vif2netif[WL80211_VIF_AP])

static int init_result = 1; /* 1: uninitialized; 0: initialized; -1: failed. */
static volatile bool init_task_done;

static volatile enum wifi_port_state state = WIFI_PORT_IDLE;
static volatile bool connecting;
static volatile bool got_ip;
static volatile bool user_disconnect;
static volatile bool pending_disconnect; /* Connection completed after cancellation; issue the deferred disconnect. */
static volatile uint16_t last_status;
static volatile uint16_t last_reason;
static volatile uint32_t disconnect_events;
static volatile uint32_t scan_done_events;
static uint32_t cfg_ip, cfg_mask, cfg_gw, cfg_dns[2]; /* Use DHCP when cfg_ip is zero. */
static uint32_t ap_ip, ap_mask, ap_gw;
static bool auto_reconnect = true;
static bool reconnect_armed; /* Only after this explicit begin acquired an IP. */
static volatile bool static_pending; /* Associated; wait for link-up before applying static addresses. */
static volatile bool ap_up;            /* Between AP_STARTED and AP_STOPPED. */
static volatile bool ap_failed;
static volatile uint8_t ap_stations;
static volatile uint32_t ap_stopped_events;

static char hostname[32];
static char time_server[254] = "pool.ntp.org";
/* The public WiFi API has one caller. netifapi completes before these
   borrowed arguments go out of scope; live buffers are written on tcpip. */
static const char *pending_hostname;
static const char *pending_time_server;

/**
 * @brief Restart SNTP in the lwIP callback context after a server/configuration change.
 */
static void restart_sntp(void *arg)
{
    (void)arg;
    sntp_stop();
    sntp_init();
}

/**
 * @brief Apply the current RAM hostname to the selected lwIP interface.
 */
static void apply_hostname(struct netif *netif)
{
    memmove(hostname, pending_hostname, strlen(pending_hostname) + 1);
    netif_set_hostname(netif, hostname);
}

/**
 * @brief Configure time synchronization when the selected network interface becomes usable.
 */
static void configure_sntp(struct netif *netif)
{
    (void)netif;
    sntp_stop();
    memmove(time_server, pending_time_server, strlen(pending_time_server) + 1);
    sntp_setservername(0, time_server);
    sntp_init();
}

/**
 * @brief EV_WIFI callback. Like macsw_bare, async_event_loop runs in the timer task with the
 * scheduler suspended. Update state only; do not call blocking interfaces.
 */
static void wifi_event(async_input_event_t ev, void *priv)
{
    (void)priv;
    switch (ev->code) {
        case CODE_WIFI_ON_CONNECTING:
            if (!user_disconnect) {
                connecting = true;
                state = WIFI_PORT_IDLE;
            }
            break;
        case CODE_WIFI_ON_GOT_IP:
            if (ev->value && user_disconnect) {
                /*
 * The application timed out while disconnecting a pending connection, which completed later.
 * Suppress connected status and perform the deferred disconnect on the next interface call.
 */
                pending_disconnect = true;
                connecting = false;
            } else if (ev->value) {
                got_ip = true;
                connecting = false;
                state = WIFI_PORT_CONNECTED;
                reconnect_armed = true;
                /* SDK setter only enters a critical section; it never blocks. */
                wifi_port_set_auto_reconnect(auto_reconnect);
                tcpip_try_callback(restart_sntp, NULL);
            } else if (got_ip && cfg_ip == 0) {
                got_ip = false;
                state = WIFI_PORT_LOST;
            }
            break;
        case CODE_WIFI_ON_CONNECTED:
            if (cfg_ip != 0 && user_disconnect) {
                pending_disconnect = true; /* Handle as in GOT_IP. */
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

/**
 * @brief Drain deferred radio events while preserving the SDK scheduler exclusion convention.
 */
static void async_event_handler(void *arg1, uint32_t arg2)
{
    (void)arg1;
    (void)arg2;
    vTaskSuspendAll();
    async_event_loop();
    xTaskResumeAll();
}

/**
 * @brief Schedule deferred async-event processing with a timer-task-safe wait policy.
 */
static void async_event_loop_wake(void)
{
    TickType_t wait = portMAX_DELAY;

    if (xTimerGetTimerDaemonTaskHandle() == xTaskGetCurrentTaskHandle()) {
        wait = 0;
    }
    xTimerPendFunctionCall(async_event_handler, NULL, 0, wait);
}

/**
 * @brief Start firmware initialization from task context and signal completion through
 * platform state.
 */
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

/**
 * @brief Initialize the radio and Wi-Fi management once, reporting initialization failure.
 */
int wifi_port_init(void)
{
    if (init_result != 1) {
        return init_result;
    }
    init_result = -1;
    board_wifi_irq_init();
    if (board_radio_init() != 0) {
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
        uint8_t mac[6];
        if (wifi_mgmr_sta_mac_get(mac) != 0) {
            init_result = -1;
            return init_result;
        }
        char default_hostname[32];
        snprintf(default_hostname, sizeof(default_hostname), "bl616cl-%02X%02X%02X", mac[3], mac[4], mac[5]);
        if (wifi_port_set_hostname(default_hostname) != 0 || wifi_port_config_time(time_server) != 0) {
            init_result = -1;
        }
    }
    return init_result;
}

/**
 * @brief Request disconnect and wait for its event, bounded by timeout_ms.
 */
static void sta_disconnect_wait(uint32_t timeout_ms)
{
    uint32_t events = disconnect_events;
    wifi_mgmr_sta_disconnect();
    for (uint32_t waited = 0; disconnect_events == events && waited < timeout_ms; waited += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief Apply the selected STA static address and DNS policy to lwIP.
 */
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

/**
 * @brief Set static addresses after STA link-up, in the library caller's task.
 */
static void finish_static_ip(void)
{
    if (static_pending && netif_is_link_up(STA_NETIF)) {
        static_pending = false;
        apply_static_ip();
        got_ip = true;
        connecting = false;
        state = WIFI_PORT_CONNECTED;
        reconnect_armed = true;
        wifi_port_set_auto_reconnect(auto_reconnect);
        tcpip_try_callback(restart_sntp, NULL);
    }
}

/**
 * @brief Event callbacks cannot block; perform the deferred disconnect here in the library
 * caller's task.
 */
static void finish_pending_disconnect(void)
{
    if (pending_disconnect) {
        pending_disconnect = false;
        sta_disconnect_wait(PENDING_DISCONNECT_TIMEOUT_MS);
    }
}

/**
 * @brief Start a STA connection using the supplied SSID and optional passphrase.
 */
int wifi_port_connect(const char *ssid, const char *pass)
{
    wifi_mgmr_sta_connect_params_t p;

    finish_pending_disconnect();

    memset(&p, 0, sizeof(p));
    strncpy(p.ssid, ssid, sizeof(p.ssid) - 1);
    if (pass != NULL) {
        strncpy(p.key, pass, sizeof(p.key) - 1);
    }
    p.pmf_cfg = 1; /* PMF optional: SDK default zero cannot connect to PMF-required WPA3 APs (reason 31). */
    p.use_dhcp = cfg_ip == 0;

    user_disconnect = false;
    /* SDK retries last_connect_params (last successful SSID), which may differ
       from this begin. Initial failures must not silently join the old AP. */
    reconnect_armed = false;
    wifi_mgmr_sta_autoconnect_disable();
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

/**
 * @brief Wait for a state flag to reach its requested value or a monotonic timeout.
 */
static void wait_until(volatile bool *flag, bool value, uint32_t timeout_ms)
{
    for (uint32_t waited = 0; *flag != value && waited < timeout_ms; waited += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
        finish_static_ip();
    }
}

/**
 * @brief wifi_mgmr_sta_disconnect only affects associated stations, so wait for a pending
 * association to finish first.
 */
void wifi_port_disconnect(uint32_t timeout_ms)
{
    /* An explicit disconnect must not trigger wifi_mgmr's automatic retry. */
    reconnect_armed = false;
    wifi_mgmr_sta_autoconnect_disable();
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

/**
 * @brief Return AP state while AP is active, otherwise the STA state.
 */
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

/**
 * @brief Return STA state independently of AP activity.
 */
enum wifi_port_state wifi_port_sta_state(void)
{
    finish_pending_disconnect();
    finish_static_ip();
    return init_result == -1 ? WIFI_PORT_NO_MODULE : state;
}

/**
 * @brief Report whether the AP interface is active.
 */
bool wifi_port_ap_running(void)
{
    return ap_up;
}

/**
 * @brief Set reconnect policy for unexpected STA disconnections.
 */
int wifi_port_set_auto_reconnect(bool enable)
{
    if (init_result == 0) {
        int ret = enable && reconnect_armed && !user_disconnect ? wifi_mgmr_sta_autoconnect_enable() : wifi_mgmr_sta_autoconnect_disable();
        if (ret != 0) {
            return ret;
        }
    }
    auto_reconnect = enable;
    return 0;
}

/**
 * @brief Return the current unexpected-disconnect reconnect policy.
 */
bool wifi_port_get_auto_reconnect(void)
{
    return auto_reconnect;
}

/**
 * @brief Return UTC epoch seconds, or zero before synchronization.
 */
unsigned long wifi_port_time(void)
{
    time_t now = time(NULL);
    return now >= 946684800 ? (unsigned long)now : 0;
}

/**
 * @brief Return the current RAM-resident hostname.
 */
const char *wifi_port_hostname(void)
{
    return hostname;
}

/**
 * @brief Validate and update the DHCP hostname; return zero on success.
 */
int wifi_port_set_hostname(const char *name)
{
    pending_hostname = name;
    return netifapi_netif_common(STA_NETIF, apply_hostname, NULL);
}

/**
 * @brief Select the SNTP server and schedule synchronization when networking is ready.
 */
int wifi_port_config_time(const char *server)
{
    pending_time_server = server;
    return netifapi_netif_common(STA_NETIF, configure_sntp, NULL);
}

/**
 * @brief These run in the TCP/IP thread through netifapi, like other netif changes.
 */
static void enable_ipv6(struct netif *netif)
{
    if (ip6_addr_isinvalid(netif_ip6_addr_state(netif, 0))) {
        netif_create_ip6_linklocal_address(netif, 1);
    }
}

/**
 * @brief Enable supported STA link-local IPv6 addressing.
 */
int wifi_port_enable_ipv6(void)
{
    return init_result == 0 ? netifapi_netif_common(STA_NETIF, enable_ipv6, NULL) : -1;
}

/**
 * @brief Write the STA link-local IPv6 address as bounded text.
 */
void wifi_port_ipv6(char *address, uint32_t size)
{
    strncpy(address, "::", size);
    if (init_result != 0 || state != WIFI_PORT_CONNECTED) {
        return;
    }
    LOCK_TCPIP_CORE();
    if (ip6_addr_isvalid(netif_ip6_addr_state(STA_NETIF, 0))) {
        ip6addr_ntoa_r(netif_ip6_addr(STA_NETIF, 0), address, (int)size);
    }
    UNLOCK_TCPIP_CORE();
}

/**
 * @brief Report whether STA or AP can provide network services.
 */
bool wifi_port_net_up(void)
{
    enum wifi_port_state s = wifi_port_state();
    return s == WIFI_PORT_CONNECTED || s == WIFI_PORT_AP_LISTENING || s == WIFI_PORT_AP_CONNECTED;
}

/**
 * @brief Report whether a STA connection attempt is in progress.
 */
bool wifi_port_connecting(void)
{
    finish_static_ip();
    return connecting;
}

/**
 * @brief Copy the latest connection status and reason into optional output pointers.
 */
void wifi_port_last_error(uint16_t *status, uint16_t *reason)
{
    *status = last_status;
    *reason = last_reason;
}

/**
 * @brief A disconnect event may leave the old netif address; read it only while connected.
 */
void wifi_port_ip(uint32_t *ip, uint32_t *mask, uint32_t *gw)
{
    if (ap_up) {
        wifi_port_ap_ip(ip, mask, gw);
    } else {
        wifi_port_sta_ip(ip, mask, gw);
    }
}

/**
 * @brief Copy STA IPv4 address, mask and gateway in network byte order.
 */
void wifi_port_sta_ip(uint32_t *ip, uint32_t *mask, uint32_t *gw)
{
    uint32_t dns;
    *ip = *mask = *gw = 0;
    if (wifi_port_sta_state() == WIFI_PORT_CONNECTED) {
        wifi_mgmr_sta_ip_get(ip, mask, gw, &dns);
    }
}

/**
 * @brief Copy AP IPv4 address, mask and gateway in network byte order.
 */
void wifi_port_ap_ip(uint32_t *ip, uint32_t *mask, uint32_t *gw)
{
    *ip = *mask = *gw = 0;
    if (ap_up) {
        *ip = ip4_addr_get_u32(netif_ip4_addr(AP_NETIF));
        *mask = ip4_addr_get_u32(netif_ip4_netmask(AP_NETIF));
        *gw = ip4_addr_get_u32(netif_ip4_gw(AP_NETIF));
    }
}

/**
 * @brief SDK's wl80211 lwIP port exports this helper, but not in a public header.
 */
extern void _wifi_mgmr_ap_start_dhcpd(bool use_ipcfg, bool use_dhcpd, int start, int limit, uint32_t ip, uint32_t mask);

/**
 * @brief Apply supported AP addressing and refresh DHCP when AP is running.
 */
int wifi_port_ap_config(uint32_t ip, uint32_t mask, uint32_t gw)
{
    /* dhcpd's address pool uses the final octet; support /24 networks. */
    uint32_t host_ip = lwip_ntohl(ip);
    if (ip != 0 && (mask != PP_HTONL(0xffffff00UL) || (host_ip & 255) == 0 || (host_ip & 255) == 255)) {
        return -1;
    }
    ap_ip = ip;
    ap_mask = mask;
    ap_gw = gw;
    if (ap_up) {
        ip4_addr_t addr, netmask, gateway;
        ip4_addr_set_u32(&addr, ap_ip != 0 ? ap_ip : PP_HTONL(0xc0a80401UL));
        ip4_addr_set_u32(&netmask, ap_ip != 0 ? ap_mask : PP_HTONL(0xffffff00UL));
        ip4_addr_set_u32(&gateway, ap_ip != 0 ? ap_gw : ip4_addr_get_u32(&addr));
        uint8_t host = ip4_addr4(&addr);
        _wifi_mgmr_ap_start_dhcpd(true, true, host <= 254 - AP_POOL_SIZE ? host + 1 : host - AP_POOL_SIZE,
                                AP_POOL_SIZE, ip4_addr_get_u32(&addr), ip4_addr_get_u32(&netmask));
        return netifapi_netif_set_addr(AP_NETIF, &addr, &netmask, &gateway);
    }
    return 0;
}

/**
 * @brief Store STA addressing; a zero IP restores DHCP behavior.
 */
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
            netifapi_dhcp_stop(STA_NETIF); /* Do not treat GOT_IP zero after clearing an address as a disconnect in static mode. */
        }
        apply_static_ip();
    } else if (was_static) {
        netifapi_dhcp_start(STA_NETIF);
    }
}

/**
 * @brief Apply nonzero DNS entries while respecting DHCP/static policy.
 */
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

/**
 * @brief Return a DNS server in network byte order, or zero for an invalid index.
 */
uint32_t wifi_port_dns(int index)
{
    if (state != WIFI_PORT_CONNECTED || index < 0 || index >= DNS_MAX_SERVERS) {
        return 0;
    }
    const ip_addr_t *dns = dns_getserver((u8_t)index);
    return dns != NULL ? ip4_addr_get_u32(ip_2_ip4(dns)) : 0;
}

/**
 * @brief Return current signal strength in dBm.
 */
int32_t wifi_port_rssi(void)
{
    int rssi = 0;

    if (state != WIFI_PORT_CONNECTED || wifi_mgmr_sta_rssi_get(&rssi) != 0) {
        return 0;
    }
    return rssi;
}

/**
 * @brief Copy the AP MAC while AP is running, otherwise query the STA MAC.
 */
int wifi_port_mac(uint8_t mac[6])
{
    if (ap_up) {
        memcpy(mac, AP_NETIF->hwaddr, 6);
        return 0;
    }
    return wifi_mgmr_sta_mac_get(mac);
}

/**
 * @brief Copy the connected access point BSSID, reporting failure when unavailable.
 */
int wifi_port_bssid(uint8_t bssid[6])
{
    memset(bssid, 0, 6);
    return state == WIFI_PORT_CONNECTED ? wifi_mgmr_sta_get_bssid(bssid) : -1;
}

/**
 * @brief Match WiFiS3 encryption categories: WPA/WPA2 maps to WPA2, WPA2/WPA3 maps to WPA3.
 * wifi_mgmr ignores the privacy bit, so WEP appears open.
 */
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

/**
 * @brief Run a bounded scan and copy up to max retained access points into aps.
 */
int wifi_port_scan(struct wifi_port_ap *aps, int max, uint32_t timeout_ms)
{
    wifi_mgmr_scan_params_t p;

    if (max <= 0) {
        return -1;
    }
    finish_pending_disconnect();
    wait_until(&connecting, false, timeout_ms);
    wifi_mgmr_sta_scanlist_free(); /* Scan results accumulate between scans; clear the table first, as the SDK wifi_scan command does. */
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
    int n = (int)wifi_mgmr_sta_scanlist_dump(items, max); /* Sort by descending RSSI. */
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

/**
 * @brief Request AP stop and wait for its event, unless AP startup has not yet been reported.
 */
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

/**
 * @brief Start AP with validated credentials and association options, waiting up to
 * timeout_ms.
 */
int wifi_port_ap_start(const char *ssid, const char *pass, uint8_t channel, bool hidden, uint8_t max_connections, uint32_t timeout_ms)
{
    wifi_mgmr_ap_params_t p;
    ip4_addr_t ip, zero;

    if (ap_up) {
        ap_stop_wait(timeout_ms);
    }
    ip4_addr_set_u32(&ip, ap_ip != 0 ? ap_ip : PP_HTONL(LWIP_MAKEU32(192, 168, 4, 1)));
    uint8_t host = ip4_addr4(&ip);
    memset(&p, 0, sizeof(p));
    p.ssid = (char *)ssid;
    p.key = pass != NULL && pass[0] != '\0' ? (char *)pass : NULL; /* Use WPA2-PSK when a passphrase is supplied. */
    p.channel = channel;
    p.hidden_ssid = hidden;
    /* The pinned wl80211 has four association slots. A requested ceiling of
       five (official bridge default) therefore has an effective ceiling of four. */
    wifi_mgmr_conf_max_sta(max_connections > 4 ? 4 : max_connections);
    p.use_ipcfg = true;
    p.use_dhcpd = true;
    /*
 * dhcpd_start preserves the configured address only with an explicit pool; otherwise it changes to
 * 192.168.169.1. The pool lies around the last address octet.
 */
    p.start = host <= 254 - AP_POOL_SIZE ? host + 1 : host - AP_POOL_SIZE;
    p.limit = AP_POOL_SIZE;
    p.ap_ipaddr = ip4_addr_get_u32(&ip);
    p.ap_mask = ap_ip != 0 ? ap_mask : PP_HTONL(LWIP_MAKEU32(255, 255, 255, 0));
    ap_failed = false;
    ap_stations = 0;
    /* Clear the previous address so the following wait can detect completion of this configuration. */
    ip4_addr_set_zero(&zero);
    netifapi_netif_set_addr(AP_NETIF, &zero, &zero, &zero);
    if (wifi_mgmr_ap_start(&p) != 0) {
        ap_failed = true;
        return -1;
    }
    for (uint32_t waited = 0; waited < timeout_ms; waited += 10) {
        if (ap_up && netif_is_link_up(AP_NETIF) && ip4_addr_cmp(netif_ip4_addr(AP_NETIF), &ip)) {
            if (ap_ip != 0) {
                ip4_addr_t mask, gw;
                ip4_addr_set_u32(&mask, ap_mask);
                ip4_addr_set_u32(&gw, ap_gw);
                netifapi_netif_set_addr(AP_NETIF, &ip, &mask, &gw);
            }
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ap_stop_wait(timeout_ms);
    ap_failed = true;
    return -1;
}

/**
 * @brief Stop AP and its DHCP service within the supplied timeout.
 */
void wifi_port_ap_stop(uint32_t timeout_ms)
{
    ap_failed = false;
    if (ap_up) {
        ap_stop_wait(timeout_ms);
    }
}
