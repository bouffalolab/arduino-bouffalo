/*
 * lwIP socket 封装，接口说明见 net_port.h。
 * 本 lwIP 未开 LWIP_SO_RCVBUF，FIONREAD 不可用，所以每个 socket 配一块接收缓冲：available() 不等待地把数据读进来再计数。
 */
#include <errno.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "lwip/api.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/ip4.h"
#include "lwip/sockets.h"

#include "net_port.h"

#define RX_CACHE_SIZE 512
#define PING_ID       0xB1B1
#define PING_DATA     32

struct rx_cache {
    uint16_t start;
    uint16_t len;
    bool eof; /* 对端关闭或连接出错 */
    uint8_t buf[RX_CACHE_SIZE];
};

/* lwIP 的 socket 号为 LWIP_SOCKET_OFFSET 起的 MEMP_NUM_NETCONN 个 */
static struct rx_cache caches[MEMP_NUM_NETCONN];

static struct rx_cache *cache_of(int sock)
{
    int i = sock - LWIP_SOCKET_OFFSET;
    return i >= 0 && i < MEMP_NUM_NETCONN ? &caches[i] : NULL;
}

static uint32_t now_ms(void)
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void set_timeout(int sock, int opt, uint32_t ms)
{
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    lwip_setsockopt(sock, SOL_SOCKET, opt, &tv, sizeof(tv));
}

int net_port_resolve(const char *host, uint32_t *addr)
{
    ip_addr_t ip;

    if (host == NULL || netconn_gethostbyname(host, &ip) != ERR_OK || !IP_IS_V4(&ip)) {
        return -1;
    }
    *addr = ip4_addr_get_u32(ip_2_ip4(&ip));
    return 0;
}

int net_port_tcp_connect(uint32_t addr, uint16_t port, uint32_t timeout_ms)
{
    int sock = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct rx_cache *c = cache_of(sock);

    if (sock < 0 || c == NULL) {
        if (sock >= 0) {
            lwip_close(sock);
        }
        return -1;
    }
    c->start = c->len = 0;
    c->eof = false;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = lwip_htons(port);
    sa.sin_addr.s_addr = addr;

    /* 非阻塞连接再 select 等待，lwIP 的阻塞 connect 没有超时 */
    int flags = lwip_fcntl(sock, F_GETFL, 0);
    lwip_fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    if (lwip_connect(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        if (errno != EINPROGRESS) {
            goto fail;
        }
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(sock, &wset);
        struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
        if (lwip_select(sock + 1, NULL, &wset, NULL, &tv) <= 0) {
            goto fail;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        if (lwip_getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            goto fail;
        }
    }
    lwip_fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
    return sock;

fail:
    lwip_close(sock);
    return -1;
}

int net_port_send(int sock, const uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    size_t done = 0;

    set_timeout(sock, SO_SNDTIMEO, timeout_ms);
    while (done < len) {
        int n = lwip_send(sock, buf + done, len - done, 0);
        if (n <= 0) {
            break;
        }
        done += n;
    }
    return done > 0 || len == 0 ? (int)done : -1;
}

/* 不等待地把数据读进缓冲 */
static void fill(int sock, struct rx_cache *c)
{
    if (c->eof || c->len == RX_CACHE_SIZE) {
        return;
    }
    if (c->start > 0) {
        memmove(c->buf, c->buf + c->start, c->len);
        c->start = 0;
    }
    int n = lwip_recv(sock, c->buf + c->len, RX_CACHE_SIZE - c->len, MSG_DONTWAIT);
    if (n > 0) {
        c->len += n;
    } else if (n == 0 || (errno != EWOULDBLOCK && errno != EAGAIN)) {
        c->eof = true;
    }
}

int net_port_available(int sock)
{
    struct rx_cache *c = cache_of(sock);

    if (c == NULL) {
        return -1;
    }
    fill(sock, c);
    return c->len > 0 ? c->len : (c->eof ? -1 : 0);
}

int net_port_recv(int sock, uint8_t *buf, size_t len, bool peek)
{
    struct rx_cache *c = cache_of(sock);

    if (c == NULL) {
        return -1;
    }
    if (c->len == 0) {
        fill(sock, c);
    }
    if (c->len == 0) {
        return c->eof ? -1 : 0;
    }
    size_t n = peek ? 1 : (len < c->len ? len : c->len);
    memcpy(buf, c->buf + c->start, n);
    if (!peek) {
        c->start += n;
        c->len -= n;
    }
    return (int)n;
}

bool net_port_connected(int sock)
{
    return net_port_available(sock) >= 0;
}

int net_port_peer(int sock, uint32_t *addr, uint16_t *port)
{
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);

    if (lwip_getpeername(sock, (struct sockaddr *)&sa, &len) != 0) {
        return -1;
    }
    *addr = sa.sin_addr.s_addr;
    *port = lwip_ntohs(sa.sin_port);
    return 0;
}

void net_port_close(int sock)
{
    struct rx_cache *c = cache_of(sock);

    if (c != NULL) {
        c->start = c->len = 0;
        c->eof = false;
    }
    lwip_close(sock);
}

static void to_sockaddr(struct sockaddr_in *sa, uint32_t addr, uint16_t port)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin_len = sizeof(*sa);
    sa->sin_family = AF_INET;
    sa->sin_port = lwip_htons(port);
    sa->sin_addr.s_addr = addr;
}

int net_port_udp_open(uint32_t addr, uint16_t port)
{
    struct sockaddr_in sa;
    int sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (sock < 0) {
        return -1;
    }
    to_sockaddr(&sa, addr, port);
    if (lwip_bind(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        lwip_close(sock);
        return -1;
    }
    return sock;
}

int net_port_udp_send(int sock, uint32_t addr, uint16_t port, const uint8_t *buf, size_t len)
{
    struct sockaddr_in to;

    to_sockaddr(&to, addr, port);
    return lwip_sendto(sock, buf, len, 0, (struct sockaddr *)&to, sizeof(to)) == (int)len ? 0 : -1;
}

int net_port_udp_recv(int sock, uint8_t *buf, size_t len, uint32_t *addr, uint16_t *port)
{
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    int n = lwip_recvfrom(sock, buf, len, MSG_DONTWAIT, (struct sockaddr *)&from, &fromlen);
    if (n < 0) {
        return errno == EWOULDBLOCK || errno == EAGAIN ? 0 : -1;
    }
    *addr = from.sin_addr.s_addr;
    *port = lwip_ntohs(from.sin_port);
    return n;
}

int net_port_ping(uint32_t addr, uint8_t ttl, uint16_t seq, uint32_t timeout_ms)
{
    uint8_t pkt[sizeof(struct icmp_echo_hdr) + PING_DATA];
    uint8_t rx[64];
    struct icmp_echo_hdr *echo = (struct icmp_echo_hdr *)pkt;
    int ret = -4;

    int sock = lwip_socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        return -4;
    }
    int ttl_opt = ttl;
    lwip_setsockopt(sock, IPPROTO_IP, IP_TTL, &ttl_opt, sizeof(ttl_opt));

    ICMPH_TYPE_SET(echo, ICMP_ECHO);
    ICMPH_CODE_SET(echo, 0);
    echo->id = PING_ID;
    echo->seqno = lwip_htons(seq);
    for (size_t i = 0; i < PING_DATA; i++) {
        pkt[sizeof(*echo) + i] = (uint8_t)i;
    }
    echo->chksum = 0;
    echo->chksum = inet_chksum(pkt, sizeof(pkt));

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_len = sizeof(to);
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = addr;
    uint32_t start = now_ms();
    if (lwip_sendto(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to)) != (int)sizeof(pkt)) {
        goto out;
    }

    ret = -2;
    for (;;) {
        uint32_t spent = now_ms() - start;
        if (spent >= timeout_ms) {
            break;
        }
        set_timeout(sock, SO_RCVTIMEO, timeout_ms - spent);
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = lwip_recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &fromlen);
        if (n <= 0) {
            break;
        }
        struct ip_hdr *ip = (struct ip_hdr *)rx;
        size_t hl = IPH_HL(ip) * 4;
        if ((size_t)n < hl + sizeof(struct icmp_echo_hdr) || from.sin_addr.s_addr != addr) {
            continue;
        }
        struct icmp_echo_hdr *reply = (struct icmp_echo_hdr *)(rx + hl);
        if (ICMPH_TYPE(reply) == ICMP_ER && reply->id == PING_ID && reply->seqno == lwip_htons(seq)) {
            ret = (int)(now_ms() - start);
            break;
        }
    }
out:
    lwip_close(sock);
    return ret;
}
