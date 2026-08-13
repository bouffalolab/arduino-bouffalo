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

#endif /* BL616CL_ESP32_COMPAT_LWIP_SOCKETS_SHIM_H_ */
