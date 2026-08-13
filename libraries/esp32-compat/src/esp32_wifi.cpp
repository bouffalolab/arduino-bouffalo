#include <Arduino.h>
#include <IPAddress.h>
#include <WString.h>
#include <string.h>

#include "WiFi.h"
#include "WiFiGeneric.h"

extern "C" {
#include "wifi_mgmr_ext.h"
#include "wifi_mgmr.h"
#include "rfparam_adapter.h"
#include "async_event.h"
#include "lwip/tcpip.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
}

extern "C" void wl80211_init(void);
extern "C" void wifi_task_create(void);

#define SCAN_LIST_CAPACITY 16

/* wl80211 exposes these WIFI_STATE_* values only under CONFIG_WL80211_P2P. */
enum {
    BL_WIFI_STATE_CONNECTING = 0x02,
    BL_WIFI_STATE_CONNECTED_IP_GETTING = 0x03,
    BL_WIFI_STATE_CONNECTED_IP_GOT = 0x04
};

WiFiClass WiFi;

static bool g_init_started = false;
static bool g_mgmr_started = false;
static bool g_scan_pending = false;
static uint32_t g_scan_count = 0;
static wifi_mgmr_scan_item_t g_scan_items[SCAN_LIST_CAPACITY];
static volatile uint8_t g_sta_connected = 0;
static volatile uint8_t g_sta_got_ip = 0;
static uint32_t g_ip = 0;
static uint32_t g_mask = 0;
static uint32_t g_gw = 0;
static uint32_t g_dns = 0;
static char g_ssid[MGMR_SSID_LEN + 1];
static uint8_t g_mac[6];
static wifi_mode_t g_mode = WIFI_MODE_NULL;
static bool g_auto_reconnect = true;
static bool g_static_ip = false;
static String g_hostname;
static WiFiEventCb g_event_cb = nullptr;

static void wifi_async_event_handler(void *arg1, uint32_t arg2)
{
    (void)arg1;
    (void)arg2;
    async_event_loop();
}

static void wifi_async_event_loop_wake(void)
{
    BaseType_t ret;
    TickType_t wait = portMAX_DELAY;

    if (xTimerGetTimerDaemonTaskHandle() == xTaskGetCurrentTaskHandle()) {
        wait = 0;
    }
    ret = xTimerPendFunctionCall(wifi_async_event_handler, NULL, NULL, wait);
    configASSERT(ret == pdPASS);
}

/* The generic lwIP port config maps LWIP_RAND() to bl_rand(). */
extern "C" int bl_rand(void)
{
    static uint32_t seed = 0x12345678;
    seed = seed * 1103515245U + 12345U;
    return static_cast<int>(seed >> 16);
}

static void emit_wifi_event(int event)
{
    if (g_event_cb != nullptr) {
        g_event_cb(event);
    }
}

static void wifi_event_handler(async_input_event_t ev, void *priv)
{
    (void)priv;

    switch (ev->code) {
        case CODE_WIFI_ON_INIT_DONE:
            /* wl80211 posts INIT_DONE/MGMR_DONE after wl80211_init() returns. */
            break;
        case CODE_WIFI_ON_MGMR_DONE:
            g_mgmr_started = true;
            emit_wifi_event(ARDUINO_EVENT_WIFI_READY);
            break;
        case CODE_WIFI_ON_SCAN_DONE:
        case CODE_WIFI_ON_SCAN_DONE_ONJOIN:
            g_scan_count = wifi_mgmr_sta_scanlist_dump(g_scan_items,
                                                       SCAN_LIST_CAPACITY);
            if (g_scan_count > SCAN_LIST_CAPACITY) {
                g_scan_count = SCAN_LIST_CAPACITY;
            }
            g_scan_pending = false;
            emit_wifi_event(ARDUINO_EVENT_WIFI_SCAN_DONE);
            break;
        case CODE_WIFI_ON_CONNECTED:
            g_sta_connected = 1;
            g_sta_got_ip = 0;
            emit_wifi_event(ARDUINO_EVENT_WIFI_STA_CONNECTED);
            break;
        case CODE_WIFI_ON_GOT_IP:
            g_sta_got_ip = 1;
            wifi_mgmr_sta_ip_get(&g_ip, &g_mask, &g_gw, &g_dns);
            emit_wifi_event(ARDUINO_EVENT_WIFI_STA_GOT_IP);
            break;
        case CODE_WIFI_ON_DISCONNECT:
            g_sta_connected = 0;
            g_sta_got_ip = 0;
            emit_wifi_event(ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
            break;
        default:
            break;
    }
}

static void ensure_wifi_started(void)
{
    if (g_init_started) {
        return;
    }
    g_init_started = true;

    rfparam_init(0, NULL, 0);
    tcpip_init(NULL, NULL);
    async_event_init(wifi_async_event_loop_wake);
    async_register_event_filter(EV_WIFI, wifi_event_handler, NULL);
    wifi_task_create();
    vTaskDelay(pdMS_TO_TICKS(500));
    wl80211_init();
    wifi_mgmr_init();

    /* wifi_mgmr_init() posts INIT_DONE/MGMR_DONE asynchronously. */
    for (int i = 0; i < 500 && !g_mgmr_started; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static bool scan_item_valid(uint8_t networkItem)
{
    return networkItem < g_scan_count;
}

int WiFiClass::scanNetworks()
{
    ensure_wifi_started();
    if (!g_mgmr_started) {
        return -1;
    }

    wifi_mgmr_scan_params_t params;
    memset(&params, 0, sizeof(params));

    g_scan_count = 0;
    g_scan_pending = true;
    if (wifi_mgmr_sta_scan(&params) < 0) {
        g_scan_pending = false;
        return -1;
    }

    for (int i = 0; i < 1500 && g_scan_pending; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return static_cast<int>(g_scan_count);
}

String WiFiClass::SSID(uint8_t networkItem)
{
    if (!scan_item_valid(networkItem)) {
        return String("");
    }
    return String(g_scan_items[networkItem].ssid);
}

int32_t WiFiClass::RSSI(uint8_t networkItem)
{
    if (!scan_item_valid(networkItem)) {
        return 0;
    }
    return g_scan_items[networkItem].rssi;
}

uint8_t *WiFiClass::BSSID(uint8_t networkItem)
{
    if (!scan_item_valid(networkItem)) {
        return nullptr;
    }
    return g_scan_items[networkItem].bssid;
}

String WiFiClass::BSSIDstr(uint8_t networkItem)
{
    uint8_t *bssid = BSSID(networkItem);
    if (bssid == nullptr) {
        return String("");
    }
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    return String(buf);
}

uint8_t WiFiClass::encryptionType(uint8_t networkItem)
{
    if (!scan_item_valid(networkItem)) {
        return WIFI_AUTH_OPEN;
    }
    uint8_t auth = g_scan_items[networkItem].auth;
    if (auth <= WIFI_EVENT_BEACON_IND_AUTH_WPA2_PSK_WPA3_SAE) {
        return auth;
    }
    return WIFI_AUTH_MAX;
}

uint8_t WiFiClass::channel(uint8_t networkItem)
{
    if (!scan_item_valid(networkItem)) {
        return 0;
    }
    return g_scan_items[networkItem].channel;
}

bool WiFiClass::mode(wifi_mode_t mode)
{
    ensure_wifi_started();
    g_mode = mode;

    if (mode == WIFI_MODE_NULL) {
        wifi_mgmr_sta_disconnect();
        return true;
    }
    if ((mode == WIFI_MODE_STA) || (mode == WIFI_MODE_APSTA)) {
        return g_mgmr_started;
    }
    /* AP-only and APSTA SoftAP side are not implemented yet. */
    return false;
}

bool WiFiClass::config(IPAddress localIP, IPAddress gateway, IPAddress subnet,
                       IPAddress dns1, IPAddress dns2)
{
    ensure_wifi_started();
    if (!g_mgmr_started || localIP == IPAddress((uint32_t)0)) {
        return false;
    }

    /* wifi_mgmr_sta_ip_set() configures the static STA IPv4 parameters. */
    if (wifi_mgmr_sta_ip_set(static_cast<uint32_t>(localIP),
                             static_cast<uint32_t>(subnet),
                             static_cast<uint32_t>(gateway),
                             static_cast<uint32_t>(dns1)) < 0) {
        return false;
    }
    g_ip = static_cast<uint32_t>(localIP);
    g_mask = static_cast<uint32_t>(subnet);
    g_gw = static_cast<uint32_t>(gateway);
    g_dns = static_cast<uint32_t>(dns1);
    g_static_ip = true;
    (void)dns2;
    return true;
}

wifi_mode_t WiFiClass::getMode()
{
    return g_mode;
}

bool WiFiClass::setAutoConnect(bool autoConnect)
{
    (void)autoConnect;
    return false;
}

bool WiFiClass::getAutoConnect()
{
    return false;
}

bool WiFiClass::setAutoReconnect(bool autoReconnect)
{
    g_auto_reconnect = autoReconnect;
    return true;
}

bool WiFiClass::getAutoReconnect()
{
    return g_auto_reconnect;
}

int WiFiClass::begin(const char *ssid, const char *password)
{
    ensure_wifi_started();
    if (!g_mgmr_started || ssid == nullptr || ssid[0] == '\0') {
        return WL_CONNECT_FAILED;
    }

    g_mode = WIFI_MODE_STA;
    strncpy(g_ssid, ssid, MGMR_SSID_LEN);
    g_ssid[MGMR_SSID_LEN] = '\0';

    wifi_mgmr_sta_connect_params_t params;
    memset(&params, 0, sizeof(params));
    strncpy(params.ssid, ssid, MGMR_SSID_LEN);
    params.ssid[MGMR_SSID_LEN] = '\0';
    if (password != nullptr) {
        strncpy(params.key, password, MGMR_KEY_LEN);
        params.key[MGMR_KEY_LEN] = '\0';
    }
    params.use_dhcp = g_static_ip ? 0 : 1;
    params.pmf_cfg = 1;

    if (wifi_mgmr_sta_connect(&params) < 0) {
        return WL_CONNECT_FAILED;
    }

    /* Connection proceeds asynchronously; report idle status like ESP32. */
    return WL_IDLE_STATUS;
}

bool WiFiClass::disconnect(bool wifiOff)
{
    (void)wifiOff;
    if (!g_mgmr_started) {
        return false;
    }
    g_sta_connected = 0;
    g_sta_got_ip = 0;
    return wifi_mgmr_sta_disconnect() == 0;
}

wl_status_t WiFiClass::status()
{
    if (!g_mgmr_started) {
        return WL_NO_SHIELD;
    }
    int state = wifi_mgmr_sta_state_get();
    if (g_sta_got_ip || state == BL_WIFI_STATE_CONNECTED_IP_GOT ||
        state == BL_WIFI_STATE_CONNECTED_IP_GETTING) {
        return WL_CONNECTED;
    }
    if (state == BL_WIFI_STATE_CONNECTING) {
        return WL_IDLE_STATUS;
    }
    return WL_DISCONNECTED;
}

String WiFiClass::macAddress()
{
    if (!g_mgmr_started || wifi_mgmr_sta_mac_get(g_mac) < 0) {
        return String("");
    }
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
    return String(buf);
}

IPAddress WiFiClass::localIP()
{
    if (g_sta_got_ip) {
        return IPAddress(g_ip);
    }
    uint32_t ip = 0, mask = 0, gw = 0, dns = 0;
    if (wifi_mgmr_sta_ip_get(&ip, &mask, &gw, &dns) == 0) {
        return IPAddress(ip);
    }
    return IPAddress((uint32_t)0);
}

IPAddress WiFiClass::gatewayIP()
{
    if (g_sta_got_ip) {
        return IPAddress(g_gw);
    }
    uint32_t ip = 0, mask = 0, gw = 0, dns = 0;
    if (wifi_mgmr_sta_ip_get(&ip, &mask, &gw, &dns) == 0) {
        return IPAddress(gw);
    }
    return IPAddress((uint32_t)0);
}

IPAddress WiFiClass::subnetMask()
{
    if (g_sta_got_ip) {
        return IPAddress(g_mask);
    }
    uint32_t ip = 0, mask = 0, gw = 0, dns = 0;
    if (wifi_mgmr_sta_ip_get(&ip, &mask, &gw, &dns) == 0) {
        return IPAddress(mask);
    }
    return IPAddress((uint32_t)0);
}

IPAddress WiFiClass::dnsIP(uint8_t dnsNo)
{
    (void)dnsNo;
    if (g_sta_got_ip) {
        return IPAddress(g_dns);
    }
    uint32_t ip = 0, mask = 0, gw = 0, dns = 0;
    if (wifi_mgmr_sta_ip_get(&ip, &mask, &gw, &dns) == 0) {
        return IPAddress(dns);
    }
    return IPAddress((uint32_t)0);
}

String WiFiClass::getHostname()
{
    return g_hostname;
}

bool WiFiClass::setHostname(const char *hostname)
{
    if (hostname == nullptr) {
        return false;
    }
    g_hostname = String(hostname);
    return true;
}

IPAddress WiFiClass::localIPv6()
{
    return IPAddress((uint32_t)0);
}

void WiFiClass::enableIpV6()
{
}

bool WiFiClass::onEvent(WiFiEventCb cb)
{
    if (cb == nullptr) {
        return false;
    }
    g_event_cb = cb;
    return true;
}

void WiFiClass::persistent(bool persistent)
{
    (void)persistent;
}
