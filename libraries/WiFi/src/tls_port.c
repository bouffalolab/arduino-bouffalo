/* TLS client backend; see tls_port.h. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "lwip/sockets.h"
#include "mm.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"

#include "arduino_crt_bundle.h"
#include "bl_crt_bundle.h"
#include "net_port.h"
#include "tls_port.h"

#define CLOSE_NOTIFY_TIMEOUT_MS 1000
/*
 * Minimum free OCRAM for a new connection. mbedTLS calloc uses OCRAM before WRAM; measured
 * handshake peak is about 43-52 KiB and steady usage about 38 KiB. FreeRTOS pvPortMalloc uses only
 * OCRAM and aborts on failure. Reject below this threshold to retain about 4 KiB or more after the
 * handshake peak.
 */
#define TLS_OCRAM_MIN (56 * 1024)

struct tls_conn {
    int sock;
    bool wait;         /* Read until the deadline during handshake; read without waiting afterward. */
    uint32_t deadline; /* Milliseconds converted from ticks. */
    bool eof;          /* The peer closed or the connection failed. */
    int peeked;        /* One byte prefetched by available(), or -1 when empty. */
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
    mbedtls_x509_crt ca;
    mbedtls_x509_crt client_cert;
    mbedtls_pk_context client_key;
};

/* Index by socket number, matching net_port.c. */
static struct tls_conn *conns[MEMP_NUM_NETCONN];
static bool bundle_ready;

/**
 * @brief Find the TLS connection state associated with a socket descriptor.
 */
static struct tls_conn *conn_of(int sock)
{
    int i = sock - LWIP_SOCKET_OFFSET;
    return i >= 0 && i < MEMP_NUM_NETCONN ? conns[i] : NULL;
}

/**
 * @brief Return monotonic time in milliseconds for bounded I/O waits.
 */
static uint32_t now_ms(void)
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

/**
 * @brief Update the socket timeout used by the TLS write callback.
 */
static void set_send_timeout(int sock, uint32_t ms)
{
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/**
 * @brief Adapt a socket write to mbedTLS return and retry/error conventions.
 */
static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    struct tls_conn *t = ctx;
    int n = lwip_send(t->sock, buf, len, 0);
    if (n >= 0) {
        return n;
    }
    return errno == EWOULDBLOCK || errno == EAGAIN ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_SEND_FAILED;
}

/**
 * @brief Adapt bounded handshake or nonblocking application reads to mbedTLS conventions.
 */
static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    struct tls_conn *t = ctx;
    if (t->wait) {
        int32_t left = (int32_t)(t->deadline - now_ms());
        if (left <= 0) {
            return MBEDTLS_ERR_SSL_TIMEOUT;
        }
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(t->sock, &rset);
        struct timeval tv = { .tv_sec = left / 1000, .tv_usec = (left % 1000) * 1000 };
        int r = lwip_select(t->sock + 1, &rset, NULL, NULL, &tv);
        if (r <= 0) {
            return r == 0 ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_RECV_FAILED;
        }
    }
    int n = lwip_recv(t->sock, buf, len, MSG_DONTWAIT);
    if (n >= 0) {
        return n; /* Zero indicates peer closure. */
    }
    return errno == EWOULDBLOCK || errno == EAGAIN ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
}

/**
 * @brief Free all mbedTLS state owned by a connection slot.
 */
static void free_conn(struct tls_conn *t)
{
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->entropy);
    mbedtls_x509_crt_free(&t->ca);
    mbedtls_x509_crt_free(&t->client_cert);
    mbedtls_pk_free(&t->client_key);
    free(t);
}

/**
 * @brief Read-only SDK callback in bl_crt_bundle.c; finds roots by issuer name. Its header is
 * not exported.
 */
int bl_crt_verify_callback(void *buf, mbedtls_x509_crt *crt, int depth, uint32_t *flags);

/**
 * @brief Bundle format from bl_crt_bundle.c: big-endian 16-bit certificate count, then per
 * certificate a 16-bit name length, 16-bit public-key length, subject-name DER and SPKI DER.
 */
static bool is_bundle_root(const mbedtls_x509_crt *crt)
{
    const uint8_t *end = arduino_crt_bundle + arduino_crt_bundle_size;
    const uint8_t *p = arduino_crt_bundle + 2;
    int num = (arduino_crt_bundle[0] << 8) | arduino_crt_bundle[1];
    for (int i = 0; i < num && p + 4 <= end; i++) {
        size_t name_len = (p[0] << 8) | p[1];
        size_t key_len = (p[2] << 8) | p[3];
        const uint8_t *name = p + 4;
        const uint8_t *key = name + name_len;
        if (key + key_len > end) {
            break;
        }
        if (name_len == crt->subject_raw.len && key_len == crt->pk_raw.len &&
            memcmp(name, crt->subject_raw.p, name_len) == 0 && memcmp(key, crt->pk_raw.p, key_len) == 0) {
            return true;
        }
        p = key + key_len;
    }
    return false;
}

/**
 * @brief Trust a chain-top certificate when both its subject name and public key match a
 * bundled root. A server may send only the COMODO RSA CA cross-signed by AddTrust, whose
 * issuer is absent from the bundle; the SDK issuer lookup then fails. Local OpenSSL accepts
 * the same root-equivalence rule. Certificate date checks remain disabled by the current
 * policy.
 */
static int verify_bundle(void *data, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    (void)depth;
    int ret = bl_crt_verify_callback(data, crt, depth, flags);
    if (ret != 0 && is_bundle_root(crt)) {
        *flags = 0;
        return 0;
    }
    return ret;
}

/**
 * @brief Trust only the supplied PEM when present; otherwise use the package bundle.
 * bl_crt_bundle resolves issuers and verify_bundle handles matching root certificates.
 */
static int set_trust(struct tls_conn *t, const char *ca_pem)
{
    if (ca_pem != NULL) {
        if (mbedtls_x509_crt_parse(&t->ca, (const unsigned char *)ca_pem, strlen(ca_pem) + 1) < 0 ||
            t->ca.raw.len == 0) {
            return -1;
        }
        mbedtls_ssl_conf_ca_chain(&t->conf, &t->ca, NULL);
        return 0;
    }
    if (!bundle_ready) {
        if (bl_crt_bundle_set(arduino_crt_bundle, arduino_crt_bundle_size) != 0) {
            return -1;
        }
        bundle_ready = true;
    }
    if (bl_crt_bundle_attach(&t->conf) != 0) {
        return -1;
    }
    mbedtls_ssl_conf_verify(&t->conf, verify_bundle, NULL);
    return 0;
}

/**
 * @brief Handshake on an existing TCP socket within one deadline, releasing TLS state on
 * failure.
 */
int tls_port_open(int sock, const char *host, const char *ca_pem,
                  const char *client_cert_pem, const char *client_key_pem,
                  uint32_t timeout_ms)
{
    int i = sock - LWIP_SOCKET_OFFSET;
    if (i < 0 || i >= MEMP_NUM_NETCONN || conns[i] != NULL || kfree_size(MM_HEAP_OCRAM_0) < TLS_OCRAM_MIN) {
        return -1;
    }
    struct tls_conn *t = calloc(1, sizeof(*t));
    if (t == NULL) {
        return -1;
    }
    t->sock = sock;
    t->wait = true;
    t->deadline = now_ms() + timeout_ms;
    t->peeked = -1;
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_ctr_drbg_init(&t->drbg);
    mbedtls_entropy_init(&t->entropy);
    mbedtls_x509_crt_init(&t->ca);
    mbedtls_x509_crt_init(&t->client_cert);
    mbedtls_pk_init(&t->client_key);

    if (mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy, NULL, 0) != 0 ||
        mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0 ||
        set_trust(t, ca_pem) != 0) {
        goto fail;
    }
    if ((client_cert_pem == NULL) != (client_key_pem == NULL)) {
        goto fail;
    }
    if (client_cert_pem != NULL &&
        (mbedtls_x509_crt_parse(&t->client_cert, (const unsigned char *)client_cert_pem,
                                strlen(client_cert_pem) + 1) < 0 ||
         mbedtls_pk_parse_key(&t->client_key, (const unsigned char *)client_key_pem,
                              strlen(client_key_pem) + 1, NULL, 0, NULL, NULL) != 0 ||
         mbedtls_ssl_conf_own_cert(&t->conf, &t->client_cert, &t->client_key) != 0)) {
        goto fail;
    }
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
    if (mbedtls_ssl_setup(&t->ssl, &t->conf) != 0 || mbedtls_ssl_set_hostname(&t->ssl, host) != 0) {
        goto fail;
    }
    mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, NULL);
    set_send_timeout(sock, timeout_ms);

    int r;
    while ((r = mbedtls_ssl_handshake(&t->ssl)) != 0) {
        if ((r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) ||
            (int32_t)(t->deadline - now_ms()) <= 0) {
            goto fail;
        }
    }
    t->wait = false;
    conns[i] = t;
    return 0;

fail:
    free_conn(t);
    return -1;
}

/**
 * @brief Encrypt and transmit plaintext bytes with a bounded send wait.
 */
int tls_port_send(int sock, const uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    struct tls_conn *t = conn_of(sock);
    size_t done = 0;

    if (t == NULL) {
        return -1;
    }
    set_send_timeout(sock, timeout_ms);
    while (done < len) {
        int n = mbedtls_ssl_write(&t->ssl, buf + done, len - done);
        if (n > 0) {
            done += n;
        } else if (n != MBEDTLS_ERR_SSL_WANT_WRITE) {
            break;
        }
    }
    return done > 0 || len == 0 ? (int)done : -1;
}

/**
 * @brief Read available decrypted/arrived data without waiting. Return bytes, zero for no
 * data, or -1 on closure/error.
 */
static int read_some(struct tls_conn *t, uint8_t *buf, size_t len)
{
    if (t->eof) {
        return -1;
    }
    int n = mbedtls_ssl_read(&t->ssl, buf, len);
    if (n > 0) {
        return n;
    }
    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    t->eof = true; /* Zero, close_notify, peer disconnect or other error. */
    return -1;
}

/**
 * @brief Prefetch one byte to confirm availability, then add decrypted bytes remaining in the
 * current record.
 */
int tls_port_available(int sock)
{
    struct tls_conn *t = conn_of(sock);

    if (t == NULL) {
        return -1;
    }
    if (t->peeked < 0) {
        uint8_t b;
        int n = read_some(t, &b, 1);
        if (n <= 0) {
            return n;
        }
        t->peeked = b;
    }
    return 1 + (int)mbedtls_ssl_get_bytes_avail(&t->ssl);
}

/**
 * @brief Read or peek decrypted data without waiting.
 */
int tls_port_recv(int sock, uint8_t *buf, size_t len, bool peek)
{
    int n = tls_port_available(sock);
    if (n <= 0 || len == 0) {
        return n < 0 ? -1 : 0;
    }
    struct tls_conn *t = conn_of(sock);
    buf[0] = (uint8_t)t->peeked;
    if (peek) {
        return 1;
    }
    t->peeked = -1;
    int m = len > 1 ? read_some(t, buf + 1, len - 1) : 0;
    return 1 + (m > 0 ? m : 0);
}

/**
 * @brief Report whether a TLS session remains usable or has unread data.
 */
bool tls_port_connected(int sock)
{
    return tls_port_available(sock) >= 0;
}

/**
 * @brief Attempt bounded close_notify, free TLS state and close the underlying socket.
 */
void tls_port_close(int sock)
{
    struct tls_conn *t = conn_of(sock);

    if (t != NULL) {
        conns[sock - LWIP_SOCKET_OFFSET] = NULL;
        if (!t->eof) {
            set_send_timeout(sock, CLOSE_NOTIFY_TIMEOUT_MS);
            mbedtls_ssl_close_notify(&t->ssl);
        }
        free_conn(t);
    }
    net_port_close(sock);
}
