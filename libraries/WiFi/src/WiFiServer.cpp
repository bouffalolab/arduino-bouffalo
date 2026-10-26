/* WiFiServer：socket 操作在 net_port.c */
#include "WiFiServer.h"

#include "net_port.h"
#include "wifi_port.h"

WiFiServer::WiFiServer(uint16_t port) : port_(port)
{
    for (int &c : conns_) {
        c = -1;
    }
}

WiFiServer::~WiFiServer()
{
    end();
}

void WiFiServer::begin()
{
    begin(port_);
}

/* 未连上网络且 AP 未运行时直接失败：Wi-Fi 未初始化时 lwIP 的 tcpip 线程还不存在，不能调用 socket */
void WiFiServer::begin(uint16_t port)
{
    end();
    port_ = port;
    if (wifi_port_net_up()) {
        sock_ = net_port_tcp_listen(port);
    }
}

/* 丢掉调用者已 stop() 的连接，关闭对端已关闭且数据读完的连接，再把新连接收进空位 */
void WiFiServer::refresh()
{
    for (int i = 0; i < WIFI_SERVER_MAX_CLIENTS; i++) {
        if (conns_[i] < 0) {
            continue;
        }
        if (net_port_close_count(conns_[i]) != closes_[i]) {
            conns_[i] = -1;
        } else if (net_port_available(conns_[i]) < 0) {
            net_port_close(conns_[i]);
            conns_[i] = -1;
        }
    }
    for (int i = 0; i < WIFI_SERVER_MAX_CLIENTS; i++) {
        if (conns_[i] < 0) {
            int s = net_port_tcp_accept(sock_);
            if (s < 0) {
                break;
            }
            conns_[i] = s;
            closes_[i] = net_port_close_count(s);
        }
    }
}

WiFiClient WiFiServer::available()
{
    if (sock_ < 0) {
        return WiFiClient();
    }
    refresh();
    for (int s : conns_) {
        if (s >= 0 && net_port_available(s) > 0) {
            return WiFiClient(s);
        }
    }
    return WiFiClient();
}

WiFiClient WiFiServer::accept()
{
    int s = sock_ >= 0 ? net_port_tcp_accept(sock_) : -1;
    return s >= 0 ? WiFiClient(s) : WiFiClient();
}

size_t WiFiServer::write(uint8_t b)
{
    return write(&b, 1);
}

size_t WiFiServer::write(const uint8_t *buf, size_t size)
{
    size_t least = size;
    bool any = false;

    if (sock_ < 0) {
        return 0;
    }
    refresh();
    for (int s : conns_) {
        if (s >= 0) {
            WiFiClient c(s);
            least = min(least, c.write(buf, size));
            any = true;
        }
    }
    if (any && least < size) {
        setWriteError();
    }
    return any ? least : 0;
}

/* 调用者已 stop() 的连接号可能已给了别的连接，不能再关 */
void WiFiServer::end()
{
    for (int i = 0; i < WIFI_SERVER_MAX_CLIENTS; i++) {
        if (conns_[i] >= 0 && net_port_close_count(conns_[i]) == closes_[i]) {
            net_port_close(conns_[i]);
        }
        conns_[i] = -1;
    }
    if (sock_ >= 0) {
        net_port_close(sock_);
        sock_ = -1;
    }
}

WiFiServer::operator bool()
{
    return sock_ >= 0;
}
