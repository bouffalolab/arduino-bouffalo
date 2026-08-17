#ifndef BL616CL_ESP32_COMPAT_LWIP_SOCKETS_SHIM_H_
#define BL616CL_ESP32_COMPAT_LWIP_SOCKETS_SHIM_H_

/*
 * Include the lwIP BSD socket headers and then drop the compat macros.
 * lwIP's LWIP_COMPAT_SOCKETS/LWIP_POSIX_SOCKETS_IO_NAMES define `read`,
 * `write`, `connect`, ... which collide with the Arduino Stream/Print API.
 * The lwip_* functions and sockaddr types remain available.
 */

extern "C" {
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
}

#undef accept
#undef bind
#undef shutdown
#undef getpeername
#undef getsockname
#undef setsockopt
#undef getsockopt
#undef closesocket
#undef connect
#undef listen
#undef recv
#undef recvmsg
#undef recvfrom
#undef send
#undef sendmsg
#undef sendto
#undef socket
#undef select
#undef poll
#undef ioctlsocket
#undef inet_ntop
#undef inet_pton
#undef read
#undef readv
#undef write
#undef writev
#undef close
#undef fcntl
#undef ioctl

/* lwip/inet.h provides the POSIX INADDR_NONE macro, while the Arduino core
 * declares a global `INADDR_NONE` object in IPAddress.h.  Keep the core
 * symbol. */
#undef INADDR_NONE

/* Resolve a host name or a dotted-quad address string to a sockaddr_in.
 *
 * lwIP's lwip_getaddrinfo() parses numeric address strings only when
 * AI_NUMERICHOST is set; otherwise it always consults DNS.  The UNO R4
 * bridge AT commands pass numeric IPs as strings, so try the numeric form
 * first and fall back to DNS for real host names. */
static inline bool lwip_resolve_host(const char *host, uint16_t port, int socktype,
                                     struct sockaddr_in *out)
{
    struct addrinfo hints;
    struct addrinfo *results = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = socktype;

    hints.ai_flags = AI_NUMERICHOST;
    int rc = lwip_getaddrinfo(host, nullptr, &hints, &results);
    if (rc != 0 || results == nullptr) {
        hints.ai_flags = 0;
        rc = lwip_getaddrinfo(host, nullptr, &hints, &results);
    }
    if (rc != 0 || results == nullptr) {
        return false;
    }

    memcpy(out, results->ai_addr, sizeof(struct sockaddr_in));
    out->sin_port = lwip_htons(port);
    lwip_freeaddrinfo(results);
    return true;
}

#endif /* BL616CL_ESP32_COMPAT_LWIP_SOCKETS_SHIM_H_ */
