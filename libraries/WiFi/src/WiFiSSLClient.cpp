/* WiFiSSLClient delegates TCP to net_port.c and TLS to tls_port.c. */
#include "WiFiSSLClient.h"

#include "net_port.h"
#include "tls_port.h"
#include "wifi_port.h"

#define CONNECT_TIMEOUT_MS 10000 /* Use the same timeout as WiFiClient. */
#define SEND_TIMEOUT_MS    10000

/**
 * @brief Open TCP then authenticate a bounded TLS handshake using the selected trust and
 * optional client credentials.
 */
int WiFiSSLClient::connect(IPAddress ip, uint16_t port)
{
    return connect(ip.toString().c_str(), port);
}

/**
 * @brief Fail without a network, for the same reason as WiFiClient::connect.
 */
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
    if (used >= timeout || tls_port_open(sock, host, root_ca_, certificate_, private_key_, timeout - used) != 0) {
        net_port_close(sock);
        return 0;
    }
    sock_ = sock;
    return 1;
}

/**
 * @brief Select the caller-owned PEM roots used by future TLS connections; nullptr restores
 * bundled roots.
 */
void WiFiSSLClient::setCACert(const char *root_ca)
{
    root_ca_ = root_ca;
}

/**
 * @brief Select the caller-owned client certificate used by future TLS handshakes.
 */
void WiFiSSLClient::setCertificate(const char *certificate)
{
    certificate_ = certificate;
}

/**
 * @brief Select the caller-owned client private key used by future TLS handshakes.
 */
void WiFiSSLClient::setPrivateKey(const char *private_key)
{
    private_key_ = private_key;
}

/**
 * @brief Encrypt and send bytes with a bounded wait, returning the accepted plaintext count.
 */
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

/**
 * @brief Return the currently readable byte count without consuming input.
 */
int WiFiSSLClient::available()
{
    if (sock_ < 0) {
        return 0;
    }
    int n = tls_port_available(sock_);
    return n > 0 ? n : 0;
}

/**
 * @brief Consume available input bytes; return the byte/count or the interface-specific
 * empty/error value.
 */
int WiFiSSLClient::read(uint8_t *buf, size_t size)
{
    if (sock_ < 0 || size == 0) {
        return -1;
    }
    int n = tls_port_recv(sock_, buf, size, false);
    return n > 0 ? n : -1;
}

/**
 * @brief Return the next input byte without consuming it, or -1 when none is available.
 */
int WiFiSSLClient::peek()
{
    uint8_t c;
    if (sock_ < 0 || tls_port_recv(sock_, &c, 1, true) != 1) {
        return -1;
    }
    return c;
}

/**
 * @brief Attempt bounded TLS close_notify, release session state and close TCP.
 */
void WiFiSSLClient::stop()
{
    if (sock_ >= 0) {
        tls_port_close(sock_);
        sock_ = -1;
    }
}

/**
 * @brief Report whether the client is connected or retains unread received data.
 */
uint8_t WiFiSSLClient::connected()
{
    return sock_ >= 0 && tls_port_connected(sock_);
}
