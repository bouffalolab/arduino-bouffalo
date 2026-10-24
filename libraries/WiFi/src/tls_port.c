/* TLS 客户端，接口说明见 tls_port.h */
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
#include "mbedtls/ssl.h"

#include "arduino_crt_bundle.h"
#include "bl_crt_bundle.h"
#include "net_port.h"
#include "tls_port.h"

#define CLOSE_NOTIFY_TIMEOUT_MS 1000
/*
 * 新连接要求的 OCRAM 空闲量。mbedTLS 的 calloc 先用 OCRAM 再用 WRAM，实测一次握手峰值约 43–52 KB、之后常驻约 38 KB；
 * FreeRTOS 的 pvPortMalloc 只从 OCRAM 分配，失败即 abort。低于此值时拒绝连接，握手峰值后 OCRAM 仍约留 4 KB 以上
 */
#define TLS_OCRAM_MIN (56 * 1024)

struct tls_conn {
    int sock;
    bool wait;         /* 握手时读等到 deadline；之后读不等待 */
    uint32_t deadline; /* tick 换算的毫秒 */
    bool eof;          /* 对端关闭或连接出错 */
    int peeked;        /* available() 预读的 1 字节，没有时为 -1 */
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
    mbedtls_x509_crt ca;
};

/* 按 socket 号索引，同 net_port.c */
static struct tls_conn *conns[MEMP_NUM_NETCONN];
static bool bundle_ready;

static struct tls_conn *conn_of(int sock)
{
    int i = sock - LWIP_SOCKET_OFFSET;
    return i >= 0 && i < MEMP_NUM_NETCONN ? conns[i] : NULL;
}

static uint32_t now_ms(void)
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void set_send_timeout(int sock, uint32_t ms)
{
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    struct tls_conn *t = ctx;
    int n = lwip_send(t->sock, buf, len, 0);
    if (n >= 0) {
        return n;
    }
    return errno == EWOULDBLOCK || errno == EAGAIN ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_SEND_FAILED;
}

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
        return n; /* 0 为对端关闭 */
    }
    return errno == EWOULDBLOCK || errno == EAGAIN ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
}

static void free_conn(struct tls_conn *t)
{
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->entropy);
    mbedtls_x509_crt_free(&t->ca);
    free(t);
}

/* 给定 PEM 时只信任它，否则用包内根证书束（bl_crt_bundle 的校验回调按签发者查找根证书） */
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
    return bl_crt_bundle_attach(&t->conf);
}

int tls_port_open(int sock, const char *host, const char *ca_pem, uint32_t timeout_ms)
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

    if (mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy, NULL, 0) != 0 ||
        mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0 ||
        set_trust(t, ca_pem) != 0) {
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

/* 不等待地取已解密或已到达的数据；返回字节数，暂无数据返回 0，连接已关闭或出错返回 -1 */
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
    t->eof = true; /* 0、close_notify、对端断开或出错 */
    return -1;
}

/* 先预读 1 字节确认有数据，再加上当前记录中已解密的部分 */
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

bool tls_port_connected(int sock)
{
    return tls_port_available(sock) >= 0;
}

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
