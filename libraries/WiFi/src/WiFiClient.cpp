/* WiFiClient：socket 操作在 net_port.c */
#include "WiFiClient.h"

#include "net_port.h"
#include "wifi_port.h"

#define CONNECT_TIMEOUT_MS 10000
#define SEND_TIMEOUT_MS    10000 /* 对端长时间不收时 write() 返回已发出的部分 */

WiFiClient::WiFiClient()
{
}

WiFiClient::WiFiClient(int sock) : sock_(sock)
{
}

/* 未连上网络且 AP 未运行时直接失败：Wi-Fi 未初始化时 lwIP 的 tcpip 线程还不存在，不能调用 socket */
int WiFiClient::connect(IPAddress ip, uint16_t port)
{
    stop();
    if (!wifi_port_net_up()) {
        return 0;
    }
    uint32_t addr = (uint32_t)ip; /* IPAddress 按网络字节序保存 */
    sock_ = net_port_tcp_connect(addr, port, connect_timeout_ > 0 ? connect_timeout_ : CONNECT_TIMEOUT_MS);
    return sock_ >= 0 ? 1 : 0;
}

int WiFiClient::connect(const char *host, uint16_t port)
{
    uint32_t addr;

    if (wifi_port_state() != WIFI_PORT_CONNECTED || net_port_resolve(host, &addr) != 0) {
        stop();
        return 0;
    }
    return connect(IPAddress(addr), port);
}

size_t WiFiClient::write(uint8_t c)
{
    return write(&c, 1);
}

size_t WiFiClient::write(const uint8_t *buf, size_t size)
{
    if (sock_ < 0) {
        setWriteError();
        return 0;
    }
    int n = net_port_send(sock_, buf, size, SEND_TIMEOUT_MS);
    if (n < (int)size) {
        setWriteError();
    }
    return n > 0 ? n : 0;
}

int WiFiClient::available()
{
    if (sock_ < 0) {
        return 0;
    }
    int n = net_port_available(sock_);
    return n > 0 ? n : 0;
}

int WiFiClient::read()
{
    uint8_t c;
    return read(&c, 1) == 1 ? c : -1;
}

int WiFiClient::read(uint8_t *buf, size_t size)
{
    if (sock_ < 0 || size == 0) {
        return -1;
    }
    int n = net_port_recv(sock_, buf, size, false);
    return n > 0 ? n : -1;
}

int WiFiClient::peek()
{
    uint8_t c;
    if (sock_ < 0 || net_port_recv(sock_, &c, 1, true) != 1) {
        return -1;
    }
    return c;
}

/* 发送不经缓冲，没有要等待的数据 */
void WiFiClient::flush()
{
}

void WiFiClient::stop()
{
    if (sock_ >= 0) {
        net_port_close(sock_);
        sock_ = -1;
    }
}

uint8_t WiFiClient::connected()
{
    return sock_ >= 0 && net_port_connected(sock_);
}

WiFiClient::operator bool()
{
    return sock_ >= 0;
}

IPAddress WiFiClient::remoteIP()
{
    uint32_t addr = 0;
    uint16_t port = 0;
    if (sock_ < 0 || net_port_peer(sock_, &addr, &port) != 0) {
        return IPAddress((uint32_t)0);
    }
    return IPAddress(addr);
}

uint16_t WiFiClient::remotePort()
{
    uint32_t addr = 0;
    uint16_t port = 0;
    if (sock_ < 0 || net_port_peer(sock_, &addr, &port) != 0) {
        return 0;
    }
    return port;
}

IPAddress WiFiClient::localIP()
{
    uint32_t addr = 0;
    uint16_t port = 0;
    if (sock_ < 0 || net_port_local(sock_, &addr, &port) != 0) {
        return IPAddress((uint32_t)0);
    }
    return IPAddress(addr);
}

uint16_t WiFiClient::localPort()
{
    uint32_t addr = 0;
    uint16_t port = 0;
    if (sock_ < 0 || net_port_local(sock_, &addr, &port) != 0) {
        return 0;
    }
    return port;
}

void WiFiClient::setConnectionTimeout(int timeout)
{
    connect_timeout_ = timeout;
}
