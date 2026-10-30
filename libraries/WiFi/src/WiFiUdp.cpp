/* WiFiUDP delegates socket operations to net_port.c. */
#include <stdlib.h>
#include <string.h>

#include "WiFiUdp.h"

#include "net_port.h"
#include "wifi_port.h"

/**
 * @brief Close the UDP socket and release packet state.
 */
WiFiUDP::~WiFiUDP()
{
    stop();
}

/**
 * @brief Fail without a network or active AP: lwIP tcpip does not exist before Wi-Fi
 * initialization.
 */
bool WiFiUDP::open(uint32_t addr, uint16_t port)
{
    stop();
    if (!wifi_port_net_up()) {
        return false;
    }
    rx_ = static_cast<uint8_t *>(malloc(2 * WIFI_UDP_BUFFER_SIZE));
    if (rx_ == nullptr) {
        return false;
    }
    tx_ = rx_ + WIFI_UDP_BUFFER_SIZE;
    sock_ = net_port_udp_open(addr, port);
    if (sock_ < 0) {
        stop();
        return false;
    }
    return true;
}

/**
 * @brief Bind a UDP socket to the requested local address and port; return 1 on success.
 */
uint8_t WiFiUDP::begin(uint16_t port)
{
    return open(0, port) ? 1 : 0;
}

/**
 * @brief Bind a UDP socket to the requested local address and port; return 1 on success.
 */
uint8_t WiFiUDP::begin(IPAddress ip, uint16_t port)
{
    return open((uint32_t)ip, port) ? 1 : 0;
}

/**
 * @brief Bind a nonzero UDP port and join the specified IPv4 multicast group.
 */
uint8_t WiFiUDP::beginMulticast(IPAddress ip, uint16_t port)
{
    /* lwIP's implicit all-hosts group cannot be joined explicitly (SDK asserts). */
    if (ip[0] < 224 || ip[0] > 239 || ip == IPAddress(224, 0, 0, 1) || port == 0 || !open(0, port)) {
        return 0;
    }
    if (net_port_udp_join(sock_, (uint32_t)ip) != 0) {
        stop();
        return 0;
    }
    multicast_addr_ = (uint32_t)ip;
    multicast_port_ = port;
    return 1;
}

/**
 * @brief Select this socket's joined multicast group as the pending destination.
 */
int WiFiUDP::beginMulticastPacket()
{
    return multicast_port_ != 0 ? beginPacket(IPAddress(multicast_addr_), multicast_port_) : 0;
}

/**
 * @brief Close the UDP socket and release both packet buffers and destination state.
 */
void WiFiUDP::stop()
{
    if (sock_ >= 0) {
        net_port_close(sock_);
        sock_ = -1;
    }
    free(rx_);
    rx_ = tx_ = nullptr;
    rx_len_ = rx_pos_ = 0;
    rx_addr_ = 0;
    rx_port_ = 0;
    tx_open_ = false;
    tx_len_ = 0;
    tx_addr_ = multicast_addr_ = 0;
    tx_port_ = multicast_port_ = 0;
}

/**
 * @brief Select a datagram destination, reusing the most recent one when no address is
 * supplied.
 */
int WiFiUDP::beginPacket()
{
    if (sock_ < 0 || tx_port_ == 0) {
        return 0;
    }
    tx_open_ = true;
    tx_len_ = 0;
    return 1;
}

/**
 * @brief Select a datagram destination, reusing the most recent one when no address is
 * supplied.
 */
int WiFiUDP::beginPacket(IPAddress ip, uint16_t port)
{
    if (port == 0 || (sock_ < 0 && !open(0, 0))) {
        return 0;
    }
    tx_addr_ = (uint32_t)ip; /* IPAddress stores bytes in network order. */
    tx_port_ = port;
    return beginPacket();
}

/**
 * @brief Select a datagram destination, reusing the most recent one when no address is
 * supplied.
 */
int WiFiUDP::beginPacket(const char *host, uint16_t port)
{
    uint32_t addr;

    if (wifi_port_state() != WIFI_PORT_CONNECTED || net_port_resolve(host, &addr) != 0) {
        return 0;
    }
    return beginPacket(IPAddress(addr), port);
}

/**
 * @brief Send the buffered datagram and report whether transmission was accepted.
 */
int WiFiUDP::endPacket()
{
    if (!tx_open_) {
        return 0;
    }
    tx_open_ = false;
    return net_port_udp_send(sock_, tx_addr_, tx_port_, tx_, tx_len_) == 0 ? 1 : 0;
}

/**
 * @brief Append bytes to the bounded pending datagram and return the accepted count.
 */
size_t WiFiUDP::write(uint8_t b)
{
    return write(&b, 1);
}

/**
 * @brief Append bytes to the bounded pending datagram and return the accepted count.
 */
size_t WiFiUDP::write(const uint8_t *buffer, size_t size)
{
    if (!tx_open_) {
        setWriteError();
        return 0;
    }
    size_t n = min(size, WIFI_UDP_BUFFER_SIZE - tx_len_);
    memcpy(tx_ + tx_len_, buffer, n);
    tx_len_ += n;
    if (n < size) {
        setWriteError();
    }
    return n;
}

/**
 * @brief Fetch the next datagram without waiting, discarding unread data from the previous
 * one.
 */
int WiFiUDP::parsePacket()
{
    uint32_t addr;
    uint16_t port;

    rx_len_ = rx_pos_ = 0;
    if (sock_ < 0) {
        return 0;
    }
    int n = net_port_udp_recv(sock_, rx_, WIFI_UDP_BUFFER_SIZE, &addr, &port);
    if (n <= 0) {
        return 0;
    }
    rx_len_ = n;
    rx_addr_ = addr;
    rx_port_ = port;
    tx_addr_ = addr;
    tx_port_ = port;
    return n;
}

/**
 * @brief Return the currently readable byte count without consuming input.
 */
int WiFiUDP::available()
{
    return (int)(rx_len_ - rx_pos_);
}

/**
 * @brief Consume unread bytes from the current datagram without waiting.
 */
int WiFiUDP::read()
{
    return rx_pos_ < rx_len_ ? rx_[rx_pos_++] : -1;
}

/**
 * @brief Consume unread bytes from the current datagram without waiting.
 */
int WiFiUDP::read(unsigned char *buffer, size_t len)
{
    size_t n = min(len, rx_len_ - rx_pos_);
    if (n > 0) {
        memcpy(buffer, rx_ + rx_pos_, n);
        rx_pos_ += n;
    }
    return (int)n;
}

/**
 * @brief Consume unread bytes from the current datagram without waiting.
 */
int WiFiUDP::read(char *buffer, size_t len)
{
    return read(reinterpret_cast<unsigned char *>(buffer), len);
}

/**
 * @brief Return the next input byte without consuming it, or -1 when none is available.
 */
int WiFiUDP::peek()
{
    return rx_pos_ < rx_len_ ? rx_[rx_pos_] : -1;
}

/**
 * @brief Discard the unread part of the current received datagram.
 */
void WiFiUDP::flush()
{
    rx_pos_ = rx_len_;
}

/**
 * @brief Return the connected peer or current datagram source address.
 */
IPAddress WiFiUDP::remoteIP()
{
    return IPAddress(rx_addr_);
}

/**
 * @brief Return the connected peer or current datagram source port.
 */
uint16_t WiFiUDP::remotePort()
{
    return rx_port_;
}
