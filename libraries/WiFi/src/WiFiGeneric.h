/*
WiFiGeneric.h - Bouffalolab Wifi support.

Copyright (c) 2023 BH6BAO <qqwang@bouffalolab.com>
Copyright (c) 2023 Bouffalo Lab Intelligent Technology (Nanjing) Co., Ltd. All rights reserved.

*/

#ifndef BOUFFALOLAB_WIFI_GENERIC_H__
#define BOUFFALOLAB_WIFI_GENERIC_H__

#include "WiFiType.h"
#include "IPAddress.h"

typedef enum {
	ARDUINO_EVENT_WIFI_READY = 0,
	ARDUINO_EVENT_WIFI_SCAN_DONE,
	ARDUINO_EVENT_WIFI_STA_START,
	ARDUINO_EVENT_WIFI_STA_STOP,
	ARDUINO_EVENT_WIFI_STA_CONNECTED,
	ARDUINO_EVENT_WIFI_STA_DISCONNECTED,
	ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE,
	ARDUINO_EVENT_WIFI_STA_GOT_IP,
	ARDUINO_EVENT_WIFI_STA_GOT_IP6,
	ARDUINO_EVENT_WIFI_STA_LOST_IP,
	ARDUINO_EVENT_WIFI_AP_START,
	ARDUINO_EVENT_WIFI_AP_STOP,
	ARDUINO_EVENT_WIFI_AP_STACONNECTED,
	ARDUINO_EVENT_WIFI_AP_STADISCONNECTED,
	ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED,
	ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED,
	ARDUINO_EVENT_WIFI_AP_GOT_IP6,
	ARDUINO_EVENT_WIFI_FTM_REPORT,
	ARDUINO_EVENT_ETH_START,
	ARDUINO_EVENT_ETH_STOP,
	ARDUINO_EVENT_ETH_CONNECTED,
	ARDUINO_EVENT_ETH_DISCONNECTED,
	ARDUINO_EVENT_ETH_GOT_IP,
	ARDUINO_EVENT_ETH_GOT_IP6,
	ARDUINO_EVENT_WPS_ER_SUCCESS,
	ARDUINO_EVENT_WPS_ER_FAILED,
	ARDUINO_EVENT_WPS_ER_TIMEOUT,
	ARDUINO_EVENT_WPS_ER_PIN,
	ARDUINO_EVENT_WPS_ER_PBC_OVERLAP,
	ARDUINO_EVENT_SC_SCAN_DONE,
	ARDUINO_EVENT_SC_FOUND_CHANNEL,
	ARDUINO_EVENT_SC_GOT_SSID_PSWD,
	ARDUINO_EVENT_SC_SEND_ACK_DONE,
	ARDUINO_EVENT_PROV_INIT,
	ARDUINO_EVENT_PROV_DEINIT,
	ARDUINO_EVENT_PROV_START,
	ARDUINO_EVENT_PROV_END,
	ARDUINO_EVENT_PROV_CRED_RECV,
	ARDUINO_EVENT_PROV_CRED_FAIL,
	ARDUINO_EVENT_PROV_CRED_SUCCESS,
	ARDUINO_EVENT_MAX
} arduino_event_id_t;

typedef union {
	// wifi_event_sta_scan_done_t wifi_scan_done;
	// wifi_event_sta_authmode_change_t wifi_sta_authmode_change;
	// wifi_event_sta_connected_t wifi_sta_connected;
	// wifi_event_sta_disconnected_t wifi_sta_disconnected;
	// wifi_event_sta_wps_er_pin_t wps_er_pin;
	// wifi_event_sta_wps_fail_reason_t wps_fail_reason;
	// wifi_event_ap_probe_req_rx_t wifi_ap_probereqrecved;
	// wifi_event_ap_staconnected_t wifi_ap_staconnected;
	// wifi_event_ap_stadisconnected_t wifi_ap_stadisconnected;
	// wifi_event_ftm_report_t wifi_ftm_report;
	// ip_event_ap_staipassigned_t wifi_ap_staipassigned;
	// ip_event_got_ip_t got_ip;
	// ip_event_got_ip6_t got_ip6;
	// smartconfig_event_got_ssid_pswd_t sc_got_ssid_pswd;
	// esp_eth_handle_t eth_connected;
	// wifi_sta_config_t prov_cred_recv;
	// wifi_prov_sta_fail_reason_t prov_fail_reason;
} arduino_event_info_t;

typedef struct{
	arduino_event_id_t event_id;
	arduino_event_info_t event_info;
} arduino_event_t;

typedef enum {
    WiFi_BAND_2G4 = 0,
    WiFi_BAND_5G,
    WiFi_BAND_MAX
} arduino_wifi_band_t;

static const int AP_STARTED_BIT    = BIT0;
static const int AP_HAS_IP6_BIT    = BIT1;
static const int AP_HAS_CLIENT_BIT = BIT2;
static const int STA_STARTED_BIT   = BIT3;
static const int STA_CONNECTED_BIT = BIT4;
static const int STA_HAS_IP_BIT    = BIT5;
static const int STA_HAS_IP6_BIT   = BIT6;
static const int ETH_STARTED_BIT   = BIT7;
static const int ETH_CONNECTED_BIT = BIT8;
static const int ETH_HAS_IP_BIT    = BIT9;
static const int ETH_HAS_IP6_BIT   = BIT10;
static const int WIFI_SCANNING_BIT = BIT11;
static const int WIFI_SCAN_DONE_BIT= BIT12;
static const int WIFI_DNS_IDLE_BIT = BIT13;
static const int WIFI_DNS_DONE_BIT = BIT14;

class WiFiGenericClass
{
    public:
        WiFiGenericClass();
		void tcpip_init_done(void * arg);

		static int getStatusBits();
		static int waitStatusBits(int bits, uint32_t timeout_ms);

        int32_t channel(void);
        int get_wifi_status();
        int wifi_channel_to_freq(uint8_t band = WiFi_BAND_2G4, int channel = 0);
        bool init_wifi(void);

		const char * eventName(arduino_event_id_t id);
	    static int _eventCallback(arduino_event_t *event);

	protected:
		static int setStatusBits(int bits);
    	static int clearStatusBits(int bits);

    protected:
        friend class WiFiSTAClass;
        friend class WiFiScanClass;
};

#endif // BOUFFALOLAB_WIFI_GENERIC_H__