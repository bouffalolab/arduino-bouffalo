/* WiFiSSLClient：TCP 连接在 net_port.c，TLS 在 tls_port.c */
#include "WiFiSSLClient.h"

#include "net_port.h"
#include "tls_port.h"
#include "wifi_port.h"

#define CONNECT_TIMEOUT_MS 10000 /* 同 WiFiClient */
#define SEND_TIMEOUT_MS    10000

int WiFiSSLClient::connect(IPAddress ip, uint16_t port)
{
    return connect(ip.toString().c_str(), port);
}

/* 未连上网络时直接失败，原因同 WiFiClient::connect */
int WiFiSSLClient::connect(const char *host, uint16_t port)
{
    uint32_t addr;

    stop();
    if (wifi_port_state() != WIFI_PORT_CONNECTED || net_port_resolve(host, &addr) != 0) {
        return 0;
    }
    uint32_t timeout = connect_timeout_ > 0 ? connect_timeout_ : CONNECT_TIMEOUT_MS;
    uint32_t start = millis();
    int sock = net_port_tcp_connect(addr, port, timeout);
    if (sock < 0) {
        return 0;
    }
    uint32_t used = millis() - start;
    if (used >= timeout || tls_port_open(sock, host, root_ca_, timeout - used) != 0) {
        net_port_close(sock);
        return 0;
    }
    sock_ = sock;
    return 1;
}

void WiFiSSLClient::setCACert(const char *root_ca)
{
    root_ca_ = root_ca;
}

size_t WiFiSSLClient::write(const uint8_t *buf, size_t size)
{
    if (sock_ < 0) {
        setWriteError();
        return 0;
    }
    int n = tls_port_send(sock_, buf, size, SEND_TIMEOUT_MS);
    if (n < (int)size) {
        setWriteError();
    }
    return n > 0 ? n : 0;
}

int WiFiSSLClient::available()
{
    if (sock_ < 0) {
        return 0;
    }
    int n = tls_port_available(sock_);
    return n > 0 ? n : 0;
}

int WiFiSSLClient::read(uint8_t *buf, size_t size)
{
    if (sock_ < 0 || size == 0) {
        return -1;
    }
    int n = tls_port_recv(sock_, buf, size, false);
    return n > 0 ? n : -1;
}

int WiFiSSLClient::peek()
{
    uint8_t c;
    if (sock_ < 0 || tls_port_recv(sock_, &c, 1, true) != 1) {
        return -1;
    }
    return c;
}

void WiFiSSLClient::stop()
{
    if (sock_ >= 0) {
        tls_port_close(sock_);
        sock_ = -1;
    }
}

uint8_t WiFiSSLClient::connected()
{
    return sock_ >= 0 && tls_port_connected(sock_);
}
