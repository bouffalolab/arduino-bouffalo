#ifndef BL616CL_ESP32_COMPAT_WIFICLIENT_H_
#define BL616CL_ESP32_COMPAT_WIFICLIENT_H_

#include <Arduino.h>
#include <IPAddress.h>
#include <Stream.h>

#define WIFI_CLIENT_DEF_CONN_TIMEOUT_MS 3000

class WiFiClient : public Stream {
public:
    WiFiClient();
    explicit WiFiClient(int fd);
    virtual ~WiFiClient();

    /* The socket owns its descriptor; move-only keeps ownership unambiguous. */
    WiFiClient(const WiFiClient &other) = delete;
    WiFiClient &operator=(const WiFiClient &other) = delete;
    WiFiClient(WiFiClient &&other) noexcept;
    WiFiClient &operator=(WiFiClient &&other) noexcept;

    virtual int connect(IPAddress ip, uint16_t port);
    virtual int connect(const char *host, uint16_t port);
    virtual int connect(IPAddress ip, uint16_t port, int32_t timeout);
    virtual int connect(const char *host, uint16_t port, int32_t timeout);

    virtual int available() override;
    virtual int read() override;
    virtual int read(uint8_t *buffer, size_t size);
    virtual int peek() override;
    virtual size_t write(uint8_t value) override;
    virtual size_t write(const uint8_t *buffer, size_t size) override;
    virtual int availableForWrite() override;
    virtual void flush() override;
    virtual void stop();
    virtual uint8_t connected();
    virtual operator bool();

    IPAddress remoteIP();
    uint16_t remotePort();
    IPAddress localIP();
    uint16_t localPort();
    uint8_t status();

    int fd() const { return sockfd_; }

protected:
    int sockfd_;
};

#endif
