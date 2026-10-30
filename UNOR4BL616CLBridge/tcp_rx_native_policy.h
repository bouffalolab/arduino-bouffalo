#pragma once
#include <stdint.h>
#ifndef BL616CL_TCP_RX_FAIR_WINDOW
#define BL616CL_TCP_RX_FAIR_WINDOW 0
#endif
#ifndef BL616CL_TCP_RX_MEMORY_OWNER
#define BL616CL_TCP_RX_MEMORY_OWNER 0
#endif
#if BL616CL_TCP_RX_MEMORY_OWNER && !BL616CL_TCP_RX_FAIR_WINDOW
#error "Native RX memory ownership requires fair sequence windows"
#endif
#ifndef BL616CL_TCP_RX_PHYSICAL_FAIR
#define BL616CL_TCP_RX_PHYSICAL_FAIR 0
#endif
#if BL616CL_TCP_RX_PHYSICAL_FAIR && !BL616CL_TCP_RX_MEMORY_OWNER
#error "Physical RX fairness requires native memory ownership"
#endif
namespace bridge { void tcp_rx_memory_report(uint8_t *report); void tcp_rx_physical_report(uint8_t *report); }
#if BL616CL_TCP_RX_FAIR_WINDOW
namespace bridge { bool tcp_rx_native_policy_healthy(); }
#endif
