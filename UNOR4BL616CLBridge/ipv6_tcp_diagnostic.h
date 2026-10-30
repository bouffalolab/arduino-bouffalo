#pragma once
#include <stdint.h>
#ifndef BL616CL_IPV6_TCP_DIAGNOSTIC
#define BL616CL_IPV6_TCP_DIAGNOSTIC 0
#endif
namespace bridge {
void ipv6_tcp_diagnostic_poll();
void ipv6_tcp_diagnostic_observe(bool output, void *packet);
void ipv6_tcp_diagnostic_report(uint8_t *report);
}
