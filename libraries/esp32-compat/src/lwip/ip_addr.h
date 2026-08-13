#ifndef BL616CL_ESP32_COMPAT_LWIP_IP_ADDR_H_
#define BL616CL_ESP32_COMPAT_LWIP_IP_ADDR_H_

#include <stdint.h>

typedef struct ip4_addr {
    uint32_t addr;
} ip4_addr_t;

typedef struct ip_addr {
    ip4_addr_t u_addr;
} ip_addr_t;

#define ip_2_ip4(ipaddr) (&((ipaddr)->u_addr))

#endif
