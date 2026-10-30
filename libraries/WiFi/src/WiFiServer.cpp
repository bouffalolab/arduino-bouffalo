/* WiFiServer delegates socket operations to net_port.c. */
#include "WiFiServer.h"

#include "net_port.h"
#include "wifi_port.h"

/**
 * @brief Construct an inactive TCP listener for the requested port.
 */
WiFiServer::WiFiServer(uint16_t port) : port_(port)
{
    for (int &c : conns_) {
        c = -1;
    }
}

/**
 * @brief Close the listener and its retained connections.
 */
WiFiServer::~WiFiServer()
{
    end();
}

/**
 * @brief Start the TCP listener on the selected port only when a network interface is usable.
 */
void WiFiServer::begin()
{
    begin(port_);
}

/**
 * @brief Fail without a network or active AP: the lwIP tcpip thread does not exist before Wi-
 * Fi initialization.
 */
void WiFiServer::begin(uint16_t port)
{
    end();
    port_ = port;
    if (wifi_port_net_up()) {
        sock_ = net_port_tcp_listen(port);
    }
}

/**
 * @brief Forget explicitly stopped connections, close drained peer-closed connections, then
 * accept new ones into free slots.
 */
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

/**
 * @brief Return a retained client with readable data, or an invalid client when none is ready.
 */
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

/**
 * @brief Transfer one new TCP client to the caller, which must stop it when finished.
 */
WiFiClient WiFiServer::accept()
{
    int s = sock_ >= 0 ? net_port_tcp_accept(sock_) : -1;
    return s >= 0 ? WiFiClient(s) : WiFiClient();
}

/**
 * @brief Broadcast to retained clients and return the smallest accepted count, or zero when
 * none are connected.
 */
size_t WiFiServer::write(uint8_t b)
{
    return write(&b, 1);
}

/**
 * @brief Broadcast to retained clients and return the smallest accepted count, or zero when
 * none are connected.
 */
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

/**
 * @brief An explicitly stopped socket number may have been reused by another connection; do
 * not close it again.
 */
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

/**
 * @brief Report whether this object currently refers to a usable resource.
 */
WiFiServer::operator bool()
{
    return sock_ >= 0;
}
