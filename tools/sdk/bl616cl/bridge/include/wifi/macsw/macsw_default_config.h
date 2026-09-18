#define CFG_AMSDU_4K

#if defined(BL618DG)
#define CFG_RXL_BUFFER1_AMSDU_CNT 4
#define CFG_REORD_BUF 16
#else
#define CFG_RXL_BUFFER1_AMSDU_CNT 1
#define CFG_REORD_BUF 12
#endif

#define CFG_TXDESC0 1
#define CFG_TXDESC1 32
#define CFG_TXDESC2 1
#define CFG_TXDESC3 1
#define CFG_TXDESC4 4

#define CFG_TWT 8
#define CFG_BARX 2
#define CFG_BATX 1

/*
 * UNO R4 bridge builds retain concurrent AP/STA and the default station
 * limits, but do not need the SDK default's high-throughput RX reorder
 * budget. A four-entry RX reorder window is the SDK minimum with AMPDU RX
 * enabled; it leaves a contiguous WRAM region for the SoftAP hostapd BSS
 * allocation while retaining one RX BlockAck agreement.
 */
#ifdef CONFIG_UNOR4_BRIDGE_RESOURCE_PROFILE
#undef CFG_REORD_BUF
#define CFG_REORD_BUF 4
#undef CFG_BARX
#define CFG_BARX 1
#endif
