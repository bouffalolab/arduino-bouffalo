/* WiFiClient delegates socket operations to net_port.c. */
#include "WiFiClient.h"

#include "net_port.h"
#include "wifi_port.h"

#define CONNECT_TIMEOUT_MS 10000
#define SEND_TIMEOUT_MS    10000 /* Return a partial write if the peer stops receiving for an extended period. */

/**
 * @brief Construct a client handle; accepted socket handles share explicit stop ownership.
 */
WiFiClient::WiFiClient()
{
}

/**
 * @brief Construct a client handle; accepted socket handles share explicit stop ownership.
 */
WiFiClient::WiFiClient(int sock) : sock_(sock)
{
}

/**
 * @brief Fail without an active network or AP: before Wi-Fi initialization the lwIP tcpip
 * thread does not exist, so socket calls are invalid.
 */
int WiFiClient::connect(IPAddress ip, uint16_t port)
{
    stop();
    if (!wifi_port_net_up()) {
        return 0;
    }
    uint32_t addr = (uint32_t)ip; /* IPAddress stores bytes in network order. */
    sock_ = net_port_tcp_connect(addr, port, connect_timeout_ > 0 ? connect_timeout_ : CONNECT_TIMEOUT_MS);
    return sock_ >= 0 ? 1 : 0;
}

/**
 * @brief Establish a bounded connection to the supplied address or hostname; report failure
 * when no network is usable.
 */
int WiFiClient::connect(const char *host, uint16_t port)
{
    uint32_t addr;

    if (wifi_port_state() != WIFI_PORT_CONNECTED || net_port_resolve(host, &addr) != 0) {
        stop();
        return 0;
    }
    return connect(IPAddress(addr), port);
}

/**
 * @brief Write the supplied byte or buffer and return the accepted byte count.
 */
size_t WiFiClient::write(uint8_t c)
{
    return write(&c, 1);
}

/**
 * @brief Write the supplied byte or buffer and return the accepted byte count.
 */
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

/**
 * @brief Return the currently readable byte count without consuming input.
 */
int WiFiClient::available()
{
    if (sock_ < 0) {
        return 0;
    }
    int n = net_port_available(sock_);
    return n > 0 ? n : 0;
}

/**
 * @brief Consume available input bytes; return the byte/count or the interface-specific
 * empty/error value.
 */
int WiFiClient::read()
{
    uint8_t c;
    return read(&c, 1) == 1 ? c : -1;
}

/**
 * @brief Consume available input bytes; return the byte/count or the interface-specific
 * empty/error value.
 */
int WiFiClient::read(uint8_t *buf, size_t size)
{
    if (sock_ < 0 || size == 0) {
        return -1;
    }
    int n = net_port_recv(sock_, buf, size, false);
    return n > 0 ? n : -1;
}

/**
 * @brief Return the next input byte without consuming it, or -1 when none is available.
 */
int WiFiClient::peek()
{
    uint8_t c;
    if (sock_ < 0 || net_port_recv(sock_, &c, 1, true) != 1) {
        return -1;
    }
    return c;
}

/**
 * @brief Writes are unbuffered, so there is no queued output to drain.
 */
void WiFiClient::flush()
{
}

/**
 * @brief Close the shared socket and invalidate this handle; copied handles must no longer use
 * it.
 */
void WiFiClient::stop()
{
    if (sock_ >= 0) {
        net_port_close(sock_);
        sock_ = -1;
    }
}

/**
 * @brief Report a live socket or received data still buffered after peer closure.
 */
uint8_t WiFiClient::connected()
{
    return sock_ >= 0 && net_port_connected(sock_);
}

/**
 * @brief Report whether this object currently refers to a usable resource.
 */
WiFiClient::operator bool()
{
    return sock_ >= 0;
}

/**
 * @brief Return the connected TCP peer IPv4 address, or 0.0.0.0 when unavailable.
 */
IPAddress WiFiClient::remoteIP()
{
    uint32_t addr = 0;
    uint16_t port = 0;
    if (sock_ < 0 || net_port_peer(sock_, &addr, &port) != 0) {
        return IPAddress((uint32_t)0);
    }
    return IPAddress(addr);
}

/**
 * @brief Return the connected TCP peer port, or zero when unavailable.
 */
uint16_t WiFiClient::remotePort()
{
    uint32_t addr = 0;
    uint16_t port = 0;
    if (sock_ < 0 || net_port_peer(sock_, &addr, &port) != 0) {
        return 0;
    }
    return port;
}

/**
 * @brief Return the socket local IPv4 address, or 0.0.0.0 when unavailable.
 */
IPAddress WiFiClient::localIP()
{
    uint32_t addr = 0;
    uint16_t port = 0;
    if (sock_ < 0 || net_port_local(sock_, &addr, &port) != 0) {
        return IPAddress((uint32_t)0);
    }
    return IPAddress(addr);
}

/**
 * @brief Return the socket's bound local port.
 */
uint16_t WiFiClient::localPort()
{
    uint32_t addr = 0;
    uint16_t port = 0;
    if (sock_ < 0 || net_port_local(sock_, &addr, &port) != 0) {
        return 0;
    }
    return port;
}

/**
 * @brief Set the connection deadline in milliseconds; nonpositive values select the default.
 */
void WiFiClient::setConnectionTimeout(int timeout)
{
    connect_timeout_ = timeout;
}
