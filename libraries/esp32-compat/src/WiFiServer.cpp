#include "lwip_compat_sockets.h"

#include "WiFiServer.h"

WiFiServer::WiFiServer() : port_(0), sockfd_(-1)
{
}

WiFiServer::WiFiServer(uint16_t port) : port_(port), sockfd_(-1)
{
}

WiFiServer::~WiFiServer()
{
    end();
}

void WiFiServer::begin()
{
    begin(port_);
}

void WiFiServer::begin(uint16_t port)
{
    if (sockfd_ >= 0) {
        end();
    }
    port_ = port;

    sockfd_ = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_ < 0) {
        return;
    }

    int one = 1;
    lwip_setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(port);
    addr.sin_addr.s_addr = lwip_htonl(INADDR_ANY);
    if (lwip_bind(sockfd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        end();
        return;
    }
    if (lwip_listen(sockfd_, 8) != 0) {
        end();
        return;
    }

    lwip_ioctl(sockfd_, FIONBIO, &one);
}

void WiFiServer::end()
{
    if (sockfd_ >= 0) {
        lwip_close(sockfd_);
        sockfd_ = -1;
    }
}

WiFiClient WiFiServer::accept()
{
    if (sockfd_ < 0) {
        return WiFiClient();
    }
    struct sockaddr_in addr = {};
    socklen_t addr_len = sizeof(addr);
    int client = lwip_accept(sockfd_, reinterpret_cast<struct sockaddr *>(&addr), &addr_len);
    if (client < 0) {
        return WiFiClient();
    }
    return WiFiClient(client);
}

WiFiClient WiFiServer::available()
{
    return accept();
}

WiFiServer::operator bool()
{
    return sockfd_ >= 0;
}
