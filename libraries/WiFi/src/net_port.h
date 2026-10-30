#pragma once

/*
 * Thin lwIP adapter for WiFi/WiFiClient, implemented in net_port.c. sockets.h defines
 * read/write/close/connect macros that cannot enter C++ units; it also defines struct iovec,
 * conflicting with wl80211.h, so keep it separate from wifi_port.c. Addresses use network order.
 * Use each socket from one task; LWIP_NETCONN_FULLDUPLEX is disabled.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Resolve a hostname or dotted IPv4 address. Return zero on success.
 */
int net_port_resolve(const char *host, uint32_t *addr);

/**
 * @brief Connect TCP within timeout_ms; return a socket or -1 on failure.
 */
int net_port_tcp_connect(uint32_t addr, uint16_t port, uint32_t timeout_ms);
/**
 * @brief Send within timeout_ms. Return bytes sent, or -1 if the connection fails before
 * sending any bytes.
 */
int net_port_send(int sock, const uint8_t *buf, size_t len, uint32_t timeout_ms);
/**
 * @brief Return buffered unread bytes after a nonblocking fill, capped by buffer capacity;
 * return -1 if closed and empty.
 */
int net_port_available(int sock);
/**
 * @brief Read without waiting: return bytes read, zero if temporarily empty, or -1 if closed
 * and drained. With peek, inspect one byte without consuming it.
 */
int net_port_recv(int sock, uint8_t *buf, size_t len, bool peek);
/**
 * @brief True while connected or unread data remains.
 */
bool net_port_connected(int sock);
/**
 * @brief Read the peer IPv4 address and port from a valid socket.
 */
int net_port_peer(int sock, uint32_t *addr, uint16_t *port);
/**
 * @brief Read the local IPv4 address and bound port from a valid socket.
 */
int net_port_local(int sock, uint32_t *addr, uint16_t *port);
/**
 * @brief Close the socket, clear its receive cache and increment its reuse counter.
 */
void net_port_close(int sock);
/**
 * @brief Number of net_port_close calls for this socket number. A changed count means the
 * saved connection closed, even if its number has since been reused.
 */
uint32_t net_port_close_count(int sock);

/**
 * @brief Listen for TCP on port; return the listening socket, or -1 on failure.
 */
int net_port_tcp_listen(uint16_t port);
/**
 * @brief Accept an established connection without waiting; return a socket compatible with
 * net_port_tcp_connect, or -1 if none is pending.
 */
int net_port_tcp_accept(int listen_sock);

/**
 * @brief Bind UDP to addr (zero means any address) and port (zero selects an ephemeral port).
 * Return a socket, or -1 on failure.
 */
int net_port_udp_open(uint32_t addr, uint16_t port);
/**
 * @brief Join an IPv4 group; return zero on success. lwIP leaves this socket's groups
 * automatically on close.
 */
int net_port_udp_join(int sock, uint32_t group);
/**
 * @brief Send one datagram; return zero on success.
 */
int net_port_udp_send(int sock, uint32_t addr, uint16_t port, const uint8_t *buf, size_t len);
/**
 * @brief Receive the next datagram without waiting, discarding bytes beyond len. Return bytes
 * copied, zero for no packet or an empty packet, and -1 on error.
 */
int net_port_udp_recv(int sock, uint8_t *buf, size_t len, uint32_t *addr, uint16_t *port);

/**
 * @brief Send one ICMP echo within timeout_ms. Return round-trip milliseconds, -2 for timeout
 * or -4 for error, matching WL_PING_*.
 */
int net_port_ping(uint32_t addr, uint8_t ttl, uint16_t seq, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
