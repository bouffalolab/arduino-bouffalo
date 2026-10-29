#pragma once
#include <stdint.h>
#ifndef BL616CL_STA_TCP_OWNED_RX
#define BL616CL_STA_TCP_OWNED_RX 0
#endif
// Experimental AP counterpart: keep the existing STA policy independent.
#ifndef BL616CL_AP_TCP_OWNED_RX
#define BL616CL_AP_TCP_OWNED_RX 0
#endif
#ifndef BL616CL_TCP_OOSEQ_PBUF_LIMIT
#define BL616CL_TCP_OOSEQ_PBUF_LIMIT 0
#endif
#ifndef BL616CL_TCP_OOSEQ_GLOBAL_PBUF_LIMIT
#define BL616CL_TCP_OOSEQ_GLOBAL_PBUF_LIMIT 0
#endif
#ifndef BL616CL_STA_TCP_RX_RESERVE_BYTES
#define BL616CL_STA_TCP_RX_RESERVE_BYTES 0
#endif
#ifndef BL616CL_STA_TCP_RX_CHUNK_BYTES
#define BL616CL_STA_TCP_RX_CHUNK_BYTES 0
#endif
#ifndef BL616CL_TCP_RX_ATOMIC_RESERVE
#define BL616CL_TCP_RX_ATOMIC_RESERVE 0
#endif
namespace bridge {
void *sta_rx_detach(bool is_sta, void *packet);
void udp_rx_fragment_report(uint8_t *report);
void sta_rx_ownership_report(uint8_t *report);
}
