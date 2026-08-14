#include "lwip_compat_sockets.h"

#include "WiFiUdp.h"

WiFiUDP::WiFiUDP()
    : sockfd_(-1), tx_len_(0), tx_ip_((uint32_t)0), tx_port_(0), tx_multicast_(false),
      rx_len_(0), rx_index_(0), remote_ip_((uint32_t)0), remote_port_(0),
      multicast_ip_((uint32_t)0), multicast_port_(0)
{
}

WiFiUDP::~WiFiUDP()
{
    stop();
}

uint8_t WiFiUDP::begin(uint16_t port)
{
    if (sockfd_ >= 0) {
        stop();
    }
    sockfd_ = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd_ < 0) {
        return 0;
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(port);
    addr.sin_addr.s_addr = lwip_htonl(INADDR_ANY);
    if (lwip_bind(sockfd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        stop();
        return 0;
    }
    return 1;
}

uint8_t WiFiUDP::begin(IPAddress ip, uint16_t port)
{
    if (sockfd_ >= 0) {
        stop();
    }
    sockfd_ = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd_ < 0) {
        return 0;
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(port);
    addr.sin_addr.s_addr = static_cast<uint32_t>(ip);
    if (lwip_bind(sockfd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        stop();
        return 0;
    }
    return 1;
}

uint8_t WiFiUDP::beginMulticast(IPAddress ip, uint16_t port)
{
    if (sockfd_ >= 0) {
        stop();
    }
    sockfd_ = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd_ < 0) {
        return 0;
    }

    int one = 1;
    lwip_setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(port);
    addr.sin_addr.s_addr = lwip_htonl(INADDR_ANY);
    if (lwip_bind(sockfd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        stop();
        return 0;
    }

    struct ip_mreq mreq = {};
    mreq.imr_multiaddr.s_addr = static_cast<uint32_t>(ip);
    mreq.imr_interface.s_addr = lwip_htonl(INADDR_ANY);
    if (lwip_setsockopt(sockfd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
        stop();
        return 0;
    }

    multicast_ip_ = ip;
    multicast_port_ = port;
    return 1;
}

uint8_t WiFiUDP::beginPacket()
{
    if (sockfd_ < 0) {
        return 0;
    }
    tx_len_ = 0;
    tx_multicast_ = false;
    return 1;
}

uint8_t WiFiUDP::beginMulticastPacket()
{
    if (sockfd_ < 0) {
        return 0;
    }
    tx_len_ = 0;
    tx_ip_ = multicast_ip_;
    tx_port_ = multicast_port_;
    tx_multicast_ = true;
    return 1;
}

uint8_t WiFiUDP::beginPacket(IPAddress ip, uint16_t port)
{
    if (sockfd_ < 0) {
        return 0;
    }
    tx_len_ = 0;
    tx_ip_ = ip;
    tx_port_ = port;
    tx_multicast_ = false;
    return 1;
}

uint8_t WiFiUDP::beginPacket(const char *host, uint16_t port)
{
    if (sockfd_ < 0 || host == nullptr || host[0] == '\0') {
        return 0;
    }

    struct sockaddr_in addr = {};
    if (!lwip_resolve_host(host, port, SOCK_DGRAM, &addr)) {
        return 0;
    }

    tx_len_ = 0;
    tx_ip_ = IPAddress(addr.sin_addr.s_addr);
    tx_port_ = port;
    tx_multicast_ = false;
    return 1;
}

uint8_t WiFiUDP::endPacket()
{
    if (sockfd_ < 0) {
        return 0;
    }
    if (tx_len_ == 0) {
        return 1;
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(tx_port_);
    addr.sin_addr.s_addr = static_cast<uint32_t>(tx_ip_);

    int ret = static_cast<int>(lwip_sendto(sockfd_, tx_buffer_, tx_len_, 0,
                                           reinterpret_cast<struct sockaddr *>(&addr),
                                           sizeof(addr)));
    return ret == static_cast<int>(tx_len_) ? 1 : 0;
}

int WiFiUDP::parsePacket()
{
    if (sockfd_ < 0) {
        return 0;
    }

    struct sockaddr_in addr = {};
    socklen_t addr_len = sizeof(addr);
    int ret = static_cast<int>(lwip_recvfrom(sockfd_, rx_buffer_, sizeof(rx_buffer_), 0,
                                             reinterpret_cast<struct sockaddr *>(&addr),
                                             &addr_len));
    if (ret <= 0) {
        rx_len_ = 0;
        rx_index_ = 0;
        return 0;
    }

    rx_len_ = static_cast<size_t>(ret);
    rx_index_ = 0;
    remote_ip_ = IPAddress(addr.sin_addr.s_addr);
    remote_port_ = lwip_ntohs(addr.sin_port);
    return static_cast<int>(rx_len_);
}

int WiFiUDP::available()
{
    return static_cast<int>(rx_len_ - rx_index_);
}

int WiFiUDP::read()
{
    if (rx_index_ >= rx_len_) {
        return -1;
    }
    return rx_buffer_[rx_index_++];
}

int WiFiUDP::read(uint8_t *buffer, size_t size)
{
    if (buffer == nullptr || size == 0) {
        return 0;
    }
    size_t count = rx_len_ - rx_index_;
    if (count > size) {
        count = size;
    }
    if (count == 0) {
        return -1;
    }
    memcpy(buffer, rx_buffer_ + rx_index_, count);
    rx_index_ += count;
    return static_cast<int>(count);
}

int WiFiUDP::peek()
{
    if (rx_index_ >= rx_len_) {
        return -1;
    }
    return rx_buffer_[rx_index_];
}

size_t WiFiUDP::write(uint8_t value)
{
    return write(&value, 1);
}

size_t WiFiUDP::write(const uint8_t *buffer, size_t size)
{
    if (buffer == nullptr || size == 0) {
        return 0;
    }
    size_t space = sizeof(tx_buffer_) - tx_len_;
    if (size > space) {
        size = space;
    }
    memcpy(tx_buffer_ + tx_len_, buffer, size);
    tx_len_ += size;
    return size;
}

int WiFiUDP::availableForWrite()
{
    return static_cast<int>(sizeof(tx_buffer_) - tx_len_);
}

void WiFiUDP::flush()
{
}

void WiFiUDP::stop()
{
    if (sockfd_ >= 0) {
        lwip_close(sockfd_);
        sockfd_ = -1;
    }
    tx_len_ = 0;
    rx_len_ = 0;
    rx_index_ = 0;
}

IPAddress WiFiUDP::remoteIP()
{
    return remote_ip_;
}

uint16_t WiFiUDP::remotePort()
{
    return remote_port_;
}

IPAddress WiFiUDP::localIP()
{
    struct sockaddr_in addr = {};
    socklen_t addr_len = sizeof(addr);
    if (sockfd_ < 0 || lwip_getsockname(sockfd_, reinterpret_cast<struct sockaddr *>(&addr),
                                       &addr_len) != 0) {
        return IPAddress((uint32_t)0);
    }
    return IPAddress(addr.sin_addr.s_addr);
}

uint16_t WiFiUDP::localPort()
{
    struct sockaddr_in addr = {};
    socklen_t addr_len = sizeof(addr);
    if (sockfd_ < 0 || lwip_getsockname(sockfd_, reinterpret_cast<struct sockaddr *>(&addr),
                                       &addr_len) != 0) {
        return 0;
    }
    return lwip_ntohs(addr.sin_port);
}
