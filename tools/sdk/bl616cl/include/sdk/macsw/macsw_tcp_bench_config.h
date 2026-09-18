#ifndef __MACSW_TCP_BENCH_CONFIG_H__
#define __MACSW_TCP_BENCH_CONFIG_H__

/*
 * STA-only TCP throughput benchmark profile.
 *
 * This profile intentionally trades RX/fhost resource margin for single-stream
 * shell iperf TCP TX throughput. It is selected by the wifi_tcp demo
 * high-performance build and should not be used as a general SDK Wi-Fi default.
 */

#define CFG_AMSDU_4K

#undef CFG_STA_MAX
#define CFG_STA_MAX 1

#undef CFG_BCN

#define CFG_TXDESC0 1
#define CFG_TXDESC1 32
#define CFG_TXDESC2 1
#define CFG_TXDESC3 1
#define CFG_TXDESC4 0

#define CFG_TWT 1
#define CFG_REORD_BUF 4
#define FHOST_RX_BUF_CNT 4

#endif /* __MACSW_TCP_BENCH_CONFIG_H__ */
