#pragma once
#include <stdint.h>
#ifndef BL616CL_LWIP_ALLOC_FAILURE_DIAGNOSTIC
#define BL616CL_LWIP_ALLOC_FAILURE_DIAGNOSTIC 0
#endif
namespace bridge {
void lwip_allocation_diagnostic_report(uint8_t page, uint8_t *report);
}
