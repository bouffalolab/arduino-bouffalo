#pragma once

/*
 * TLS 客户端（底层包的 mbedTLS 3.6，TLS 1.2），供 WiFiSSLClient 使用。实现在 tls_port.c：mbedTLS 与 lwIP 的头文件
 * 不进入 C++ 编译单元（同 net_port.h 的说明）。连接用 net_port_tcp_connect 建立的 socket 号标识，一个 socket 至多
 * 一个 TLS 连接。证书校验按 ticket 0029：默认用包内根证书束；链顶证书与束内根名称、公钥都相同时信任它；不查证书有效期。同一连接只在一个任务中使用。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 在已连接的 sock 上握手，最多等 timeout_ms。host 用于 SNI 与证书名称校验；ca_pem 为 NULL 时信任包内根证书束，
 * 否则只信任它（PEM，以 '\0' 结尾）。OCRAM 空闲不足一次握手所需时不开始握手。成功返回 0；失败返回 -1，并释放
 * TLS 资源，sock 由调用者关闭
 */
int tls_port_open(int sock, const char *host, const char *ca_pem,
                  const char *client_cert_pem, const char *client_key_pem,
                  uint32_t timeout_ms);
/* 加密发送，最多等 timeout_ms；返回已发送的明文字节数，未发出任何字节时返回 -1 */
int tls_port_send(int sock, const uint8_t *buf, size_t len, uint32_t timeout_ms);
/* 已解密、尚未读取的字节数（不等待）；连接已关闭且无数据时返回 -1 */
int tls_port_available(int sock);
/* 不等待地读取；返回字节数，暂无数据返回 0，连接已关闭且无数据时返回 -1。peek 为真时不取走（只看 1 字节） */
int tls_port_recv(int sock, uint8_t *buf, size_t len, bool peek);
/* 连接仍在，或还有未读数据 */
bool tls_port_connected(int sock);
/* 发送 close_notify（最多等 1 s），释放 TLS 资源并关闭 sock */
void tls_port_close(int sock);

#ifdef __cplusplus
}
#endif
