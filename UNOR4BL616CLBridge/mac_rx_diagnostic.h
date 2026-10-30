#pragma once
#include <stdint.h>
#ifndef BL616CL_MAC_RX_DIAGNOSTIC
#define BL616CL_MAC_RX_DIAGNOSTIC 0
#endif
namespace bridge {
void mac_rx_diagnostic_report(uint8_t page, uint8_t *report);
}
