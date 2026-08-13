#ifndef BL616CL_ESP32_COMPAT_LWIP_NETDB_H_
#define BL616CL_ESP32_COMPAT_LWIP_NETDB_H_

#include <stdint.h>
#include "inet.h"

typedef int socklen_t;

struct sockaddr {
    uint16_t sa_family;
    char sa_data[14];
};

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
};

struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};

#ifdef __cplusplus
extern "C" {
#endif

int getaddrinfo(const char *hostname, const char *servname,
                const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);

#ifdef __cplusplus
}
#endif

#endif
