#pragma once

/*
 * mbedTLS 3.6/TLS 1.2 backend for WiFiSSLClient. Keep mbedTLS/lwIP headers in tls_port.c rather
 * than C++ units, as for net_port.h. Identify one TLS session per connected net_port socket.
 * Default trust uses bundled roots, including exact chain-top subject/public-key matches.
 * Certificate date checks remain disabled. Use each connection from one task.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Handshake on a connected socket within timeout_ms. host supplies SNI and the
 * certificate name. NULL ca_pem selects bundled roots; otherwise use only its NUL-terminated
 * PEM. Do not begin without enough free OCRAM. Return zero on success, -1 on failure after
 * freeing TLS resources; the caller must close sock.
 */
int tls_port_open(int sock, const char *host, const char *ca_pem,
                  const char *client_cert_pem, const char *client_key_pem,
                  uint32_t timeout_ms);
/**
 * @brief Encrypt/send within timeout_ms. Return plaintext bytes sent, or -1 if nothing was
 * sent.
 */
int tls_port_send(int sock, const uint8_t *buf, size_t len, uint32_t timeout_ms);
/**
 * @brief Report decrypted unread bytes without waiting; return -1 if closed and empty.
 */
int tls_port_available(int sock);
/**
 * @brief Read without waiting: return bytes, zero if temporarily empty, or -1 if closed and
 * drained. peek inspects one byte without consuming it.
 */
int tls_port_recv(int sock, uint8_t *buf, size_t len, bool peek);
/**
 * @brief True while connected or unread data remains.
 */
bool tls_port_connected(int sock);
/**
 * @brief Send close_notify with a 1 s bound, free TLS resources and close sock.
 */
void tls_port_close(int sock);

#ifdef __cplusplus
}
#endif
