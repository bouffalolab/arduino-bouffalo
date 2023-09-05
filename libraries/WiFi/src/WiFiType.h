/*
WiFiType.h - Bouffalolab Wifi support.

Copyright (c) 2023 BH6BAO <qqwang@bouffalolab.com>
Copyright (c) 2023 Bouffalo Lab Intelligent Technology (Nanjing) Co., Ltd. All rights reserved.

*/

#ifndef BOUFFALOLAB_WIFITYPE_H__
#define BOUFFALOLAB_WIFITYPE_H__

#define WIFI_SCAN_RUNNING   (-1)
#define WIFI_SCAN_FAILED    (-2)

typedef enum {
    WIFI_MODE_NULL = 0,  /**< null mode */
    WIFI_MODE_STA,       /**< WiFi station mode */
    WIFI_MODE_AP,        /**< WiFi soft-AP mode */
    WIFI_MODE_APSTA,     /**< WiFi station + soft-AP mode */
    WIFI_MODE_MAX
} wifi_mode_t;

#define WiFiMode_t   wifi_mode_t
#define WIFI_OFF     WIFI_MODE_NULL
#define WIFI_STA     WIFI_MODE_STA
#define WIFI_AP      WIFI_MODE_AP
#define WIFI_AP_STA  WIFI_MODE_APSTA

typedef enum {
    WL_NO_SHIELD        = 255,   // for compatibility with WiFi Shield library
    WL_IDLE_STATUS      = 0,
    WL_NO_SSID_AVAIL    = 1,
    WL_SCAN_COMPLETED   = 2,
    WL_CONNECTED        = 3,
    WL_CONNECT_FAILED   = 4,
    WL_CONNECTION_LOST  = 5,
    WL_DISCONNECTED     = 6
} wl_status_t;

/* Encryption modes */
enum wl_enc_type {  /* Values map to 802.11 encryption suites... */
    ENC_TYPE_WEP  = 5,
    ENC_TYPE_TKIP = 2,
    ENC_TYPE_CCMP = 4,
    /* ... except these two, 7 and 8 are reserved in 802.11-2007 */
    ENC_TYPE_NONE = 7,
    ENC_TYPE_AUTO = 8
};

#endif // BOUFFALOLAB_WIFITYPE_H__