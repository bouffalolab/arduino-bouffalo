#pragma once

/*
 * lwIP socket 的薄封装，供 WiFi、WiFiClient 使用。实现在 net_port.c：lwIP 的 sockets.h 把 read、write、close、
 * connect 等定义成宏，不能进入 C++ 编译单元；它与 wl80211.h 都定义 struct iovec，也不能与 wifi_port.c 合并。
 * 地址均为网络字节序。同一个 socket 只在一个任务中使用（lwIP 未开 LWIP_NETCONN_FULLDUPLEX）。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 域名或点分十进制地址解析为 IPv4；成功返回 0 */
int net_port_resolve(const char *host, uint32_t *addr);

/* 建立 TCP 连接，最多等 timeout_ms；成功返回 socket，失败返回 -1 */
int net_port_tcp_connect(uint32_t addr, uint16_t port, uint32_t timeout_ms);
/* 发送，最多等 timeout_ms；返回已发送的字节数，连接出错且未发出任何字节时返回 -1 */
int net_port_send(int sock, const uint8_t *buf, size_t len, uint32_t timeout_ms);
/* 已收到、尚未读取的字节数（读入内部缓冲后计数，最多为缓冲大小）；连接已关闭且无数据时返回 -1 */
int net_port_available(int sock);
/* 不等待地读取；返回字节数，暂无数据返回 0，连接已关闭且无数据时返回 -1。peek 为真时不取走（只看 1 字节） */
int net_port_recv(int sock, uint8_t *buf, size_t len, bool peek);
/* 连接仍在，或还有未读数据 */
bool net_port_connected(int sock);
int net_port_peer(int sock, uint32_t *addr, uint16_t *port);
void net_port_close(int sock);

/* UDP socket，绑定本地地址 addr（0 为任意地址）与端口 port（0 由协议栈分配）；成功返回 socket，失败返回 -1 */
int net_port_udp_open(uint32_t addr, uint16_t port);
/* 发一个数据报；成功返回 0 */
int net_port_udp_send(int sock, uint32_t addr, uint16_t port, const uint8_t *buf, size_t len);
/* 不等待地取下一个数据报，超出 len 的部分丢弃；返回写入 buf 的字节数，没有数据报（或收到空数据报）返回 0，出错返回 -1 */
int net_port_udp_recv(int sock, uint8_t *buf, size_t len, uint32_t *addr, uint16_t *port);

/* ICMP echo 一次，最多等 timeout_ms；返回往返毫秒数，超时返回 -2，出错返回 -4（取值同 WL_PING_*） */
int net_port_ping(uint32_t addr, uint8_t ttl, uint16_t seq, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
