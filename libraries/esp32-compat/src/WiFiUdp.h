#ifndef BL616CL_ESP32_COMPAT_WIFIUDP_H_
#define BL616CL_ESP32_COMPAT_WIFIUDP_H_

#include <Arduino.h>
#include <IPAddress.h>
#include <Stream.h>

#define WIFI_UDP_TX_BUFFER_SIZE 1460
#define WIFI_UDP_RX_BUFFER_SIZE 1460

class WiFiUDP : public Stream {
public:
    WiFiUDP();
    virtual ~WiFiUDP();

    uint8_t begin(uint16_t port);
    uint8_t begin(IPAddress ip, uint16_t port);
    uint8_t beginMulticast(IPAddress ip, uint16_t port);

    uint8_t beginPacket();
    uint8_t beginMulticastPacket();
    uint8_t beginPacket(IPAddress ip, uint16_t port);
    uint8_t beginPacket(const char *host, uint16_t port);
    uint8_t endPacket();

    int parsePacket();
    virtual int available() override;
    virtual int read() override;
    virtual int read(uint8_t *buffer, size_t size);
    virtual int peek() override;
    virtual size_t write(uint8_t value) override;
    virtual size_t write(const uint8_t *buffer, size_t size) override;
    virtual int availableForWrite() override;
    virtual void flush() override;
    virtual void stop();

    IPAddress remoteIP();
    uint16_t remotePort();
    IPAddress localIP();
    uint16_t localPort();

    int fd() const { return sockfd_; }

private:
    int sockfd_;
    uint8_t tx_buffer_[WIFI_UDP_TX_BUFFER_SIZE];
    size_t tx_len_;
    IPAddress tx_ip_;
    uint16_t tx_port_;
    bool tx_multicast_;
    uint8_t rx_buffer_[WIFI_UDP_RX_BUFFER_SIZE];
    size_t rx_len_;
    size_t rx_index_;
    IPAddress remote_ip_;
    uint16_t remote_port_;
    IPAddress multicast_ip_;
    uint16_t multicast_port_;
};

#endif
