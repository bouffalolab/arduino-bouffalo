#ifndef BL616CL_ESP32_COMPAT_WIFIUDP_H_
#define BL616CL_ESP32_COMPAT_WIFIUDP_H_

#include <Arduino.h>
#include <IPAddress.h>
#include <Stream.h>

class WiFiUDP : public Stream {
public:
    WiFiUDP() {}
    virtual ~WiFiUDP() {}

    uint8_t begin(uint16_t port) { (void)port; return 0; }
    uint8_t begin(IPAddress ip, uint16_t port) { (void)ip; (void)port; return 0; }
    uint8_t beginMulticast(IPAddress ip, uint16_t port) { (void)ip; (void)port; return 0; }

    uint8_t beginPacket() { return 0; }
    uint8_t beginMulticastPacket() { return 0; }
    uint8_t beginPacket(IPAddress ip, uint16_t port) { (void)ip; (void)port; return 0; }
    uint8_t beginPacket(const char *host, uint16_t port) { (void)host; (void)port; return 0; }
    uint8_t endPacket() { return 0; }

    int parsePacket() { return 0; }
    virtual int available() override { return 0; }
    virtual int read() override { return -1; }
    virtual int read(uint8_t *buffer, size_t size) { (void)buffer; (void)size; return 0; }
    virtual int peek() override { return -1; }
    virtual size_t write(uint8_t value) override { (void)value; return 0; }
    virtual size_t write(const uint8_t *buffer, size_t size) override { (void)buffer; (void)size; return 0; }
    virtual int availableForWrite() override { return 0; }
    virtual void flush() override {}
    virtual void stop() {}

    IPAddress remoteIP() { return IPAddress((uint32_t)0); }
    uint16_t remotePort() { return 0; }
    IPAddress localIP() { return IPAddress((uint32_t)0); }
    uint16_t localPort() { return 0; }
};

#endif
