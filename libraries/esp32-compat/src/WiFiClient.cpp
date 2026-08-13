#include "lwip_compat_sockets.h"

#include "WiFiClient.h"

WiFiClient::WiFiClient() : sockfd_(-1)
{
}

WiFiClient::WiFiClient(int fd) : sockfd_(fd)
{
    if (sockfd_ >= 0) {
        int one = 1;
        lwip_setsockopt(sockfd_, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    }
}

WiFiClient::~WiFiClient()
{
    stop();
}

WiFiClient::WiFiClient(WiFiClient &&other) noexcept : sockfd_(other.sockfd_)
{
    other.sockfd_ = -1;
}

WiFiClient &WiFiClient::operator=(WiFiClient &&other) noexcept
{
    if (this != &other) {
        stop();
        sockfd_ = other.sockfd_;
        other.sockfd_ = -1;
    }
    return *this;
}

void WiFiClient::stop()
{
    if (sockfd_ >= 0) {
        lwip_close(sockfd_);
        sockfd_ = -1;
    }
}

static int connect_to_addr(int fd, const struct sockaddr *addr, socklen_t addrlen,
                           int32_t timeout_ms)
{
    int flags = lwip_fcntl(fd, F_GETFL, 0);
    lwip_fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int ret = lwip_connect(fd, addr, addrlen);
    if (ret == 0) {
        lwip_fcntl(fd, F_SETFL, flags);
        return 0;
    }

    unsigned long start = millis();
    for (;;) {
        fd_set write_set;
        struct timeval tv;
        int32_t remaining = timeout_ms -
                            static_cast<int32_t>(millis() - start);
        if (remaining <= 0) {
            return -1;
        }
        FD_ZERO(&write_set);
        FD_SET(fd, &write_set);
        tv.tv_sec = remaining / 1000;
        tv.tv_usec = (remaining % 1000) * 1000;

        ret = lwip_select(fd + 1, nullptr, &write_set, nullptr, &tv);
        if (ret <= 0) {
            return -1;
        }

        int so_error = 0;
        socklen_t so_error_len = sizeof(so_error);
        lwip_getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len);
        if (so_error == 0) {
            lwip_fcntl(fd, F_SETFL, flags);
            return 0;
        }
        /* 115 == lwIP's EINPROGRESS.  select may report a connecting socket
         * writable before the handshake completes, so keep waiting. */
        if (so_error != 115) {
            return -1;
        }
    }
}

int WiFiClient::connect(IPAddress ip, uint16_t port)
{
    return connect(ip, port, WIFI_CLIENT_DEF_CONN_TIMEOUT_MS);
}

int WiFiClient::connect(const char *host, uint16_t port)
{
    return connect(host, port, WIFI_CLIENT_DEF_CONN_TIMEOUT_MS);
}

int WiFiClient::connect(IPAddress ip, uint16_t port, int32_t timeout)
{
    if (sockfd_ >= 0) {
        stop();
    }

    sockfd_ = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_ < 0) {
        return 0;
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(port);
    addr.sin_addr.s_addr = lwip_htonl(static_cast<uint32_t>(ip));

    if (connect_to_addr(sockfd_, reinterpret_cast<struct sockaddr *>(&addr),
                        sizeof(addr), timeout) != 0) {
        stop();
        return 0;
    }

    int one = 1;
    lwip_setsockopt(sockfd_, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    return 1;
}

int WiFiClient::connect(const char *host, uint16_t port, int32_t timeout)
{
    if (host == nullptr || host[0] == '\0') {
        return 0;
    }

    struct addrinfo hints = {};
    struct addrinfo *results = nullptr;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (lwip_getaddrinfo(host, port_str, &hints, &results) != 0 || results == nullptr) {
        return 0;
    }

    int connected = 0;
    for (struct addrinfo *cur = results; cur != nullptr && !connected; cur = cur->ai_next) {
        if (sockfd_ >= 0) {
            stop();
        }
        sockfd_ = lwip_socket(cur->ai_family, cur->ai_socktype, cur->ai_protocol);
        if (sockfd_ < 0) {
            continue;
        }
        if (connect_to_addr(sockfd_, cur->ai_addr, cur->ai_addrlen, timeout) == 0) {
            connected = 1;
        } else {
            stop();
        }
    }
    lwip_freeaddrinfo(results);

    if (!connected) {
        stop();
        return 0;
    }
    int one = 1;
    lwip_setsockopt(sockfd_, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    return 1;
}

int WiFiClient::available()
{
    if (sockfd_ < 0) {
        return 0;
    }
    unsigned long pending = 0;
    if (lwip_ioctl(sockfd_, FIONREAD, &pending) != 0) {
        return 0;
    }
    return static_cast<int>(pending);
}

int WiFiClient::read()
{
    uint8_t value = 0;
    int ret = lwip_recv(sockfd_, &value, 1, 0);
    if (ret <= 0) {
        if (ret == 0) {
            stop();
        }
        return -1;
    }
    return value;
}

int WiFiClient::read(uint8_t *buffer, size_t size)
{
    if (buffer == nullptr || size == 0 || sockfd_ < 0) {
        return -1;
    }
    int ret = static_cast<int>(lwip_recv(sockfd_, buffer, size, 0));
    if (ret <= 0) {
        if (ret == 0) {
            stop();
        }
        return -1;
    }
    return ret;
}

int WiFiClient::peek()
{
    uint8_t value = 0;
    int ret = lwip_recv(sockfd_, &value, 1, MSG_PEEK | MSG_DONTWAIT);
    if (ret <= 0) {
        return -1;
    }
    return value;
}

size_t WiFiClient::write(uint8_t value)
{
    return write(&value, 1);
}

size_t WiFiClient::write(const uint8_t *buffer, size_t size)
{
    if (buffer == nullptr || size == 0 || sockfd_ < 0) {
        return 0;
    }
    int ret = static_cast<int>(lwip_send(sockfd_, buffer, size, 0));
    if (ret < 0) {
        return 0;
    }
    return static_cast<size_t>(ret);
}

int WiFiClient::availableForWrite()
{
    if (sockfd_ < 0) {
        return 0;
    }
    fd_set write_set;
    struct timeval tv = {0, 0};
    FD_ZERO(&write_set);
    FD_SET(sockfd_, &write_set);
    return lwip_select(sockfd_ + 1, nullptr, &write_set, nullptr, &tv) > 0 ? 1 : 0;
}

void WiFiClient::flush()
{
}

uint8_t WiFiClient::connected()
{
    if (sockfd_ < 0) {
        return 0;
    }
    uint8_t value = 0;
    int ret = lwip_recv(sockfd_, &value, 1, MSG_PEEK | MSG_DONTWAIT);
    if (ret > 0) {
        return 1;
    }
    if (ret == 0) {
        stop();
        return 0;
    }
    /* Would-block counts as connected; a real error surfaces on the next
     * read/write operation. */
    return 1;
}

WiFiClient::operator bool()
{
    return sockfd_ >= 0 && connected() != 0;
}

IPAddress WiFiClient::remoteIP()
{
    struct sockaddr_in addr = {};
    socklen_t addr_len = sizeof(addr);
    if (sockfd_ < 0 || lwip_getpeername(sockfd_, reinterpret_cast<struct sockaddr *>(&addr),
                                       &addr_len) != 0) {
        return IPAddress((uint32_t)0);
    }
    return IPAddress(static_cast<uint32_t>(lwip_ntohl(addr.sin_addr.s_addr)));
}

uint16_t WiFiClient::remotePort()
{
    struct sockaddr_in addr = {};
    socklen_t addr_len = sizeof(addr);
    if (sockfd_ < 0 || lwip_getpeername(sockfd_, reinterpret_cast<struct sockaddr *>(&addr),
                                       &addr_len) != 0) {
        return 0;
    }
    return lwip_ntohs(addr.sin_port);
}

IPAddress WiFiClient::localIP()
{
    struct sockaddr_in addr = {};
    socklen_t addr_len = sizeof(addr);
    if (sockfd_ < 0 || lwip_getsockname(sockfd_, reinterpret_cast<struct sockaddr *>(&addr),
                                       &addr_len) != 0) {
        return IPAddress((uint32_t)0);
    }
    return IPAddress(static_cast<uint32_t>(lwip_ntohl(addr.sin_addr.s_addr)));
}

uint16_t WiFiClient::localPort()
{
    struct sockaddr_in addr = {};
    socklen_t addr_len = sizeof(addr);
    if (sockfd_ < 0 || lwip_getsockname(sockfd_, reinterpret_cast<struct sockaddr *>(&addr),
                                       &addr_len) != 0) {
        return 0;
    }
    return lwip_ntohs(addr.sin_port);
}

uint8_t WiFiClient::status()
{
    return sockfd_ >= 0 ? 1 : 0;
}
