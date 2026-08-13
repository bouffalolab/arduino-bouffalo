#ifndef BL616CL_ESP32_COMPAT_LWIP_INET_H_
#define BL616CL_ESP32_COMPAT_LWIP_INET_H_

#include "ip_addr.h"

struct in_addr {
    uint32_t s_addr;
};

static inline void inet_addr_to_ip4addr(ip4_addr_t *ip4addr, const struct in_addr *inaddr)
{
    if (ip4addr != nullptr && inaddr != nullptr) {
        ip4addr->addr = inaddr->s_addr;
    }
}

#endif
