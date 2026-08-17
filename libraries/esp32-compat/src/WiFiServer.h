#ifndef BL616CL_ESP32_COMPAT_WIFISERVER_H_
#define BL616CL_ESP32_COMPAT_WIFISERVER_H_

#include <Arduino.h>
#include <Server.h>
#include "WiFiClient.h"

class WiFiServer : public Server {
public:
    WiFiServer();
    explicit WiFiServer(uint16_t port);
    virtual ~WiFiServer();

    void begin() override;
    void begin(uint16_t port);
    void end();
    WiFiClient available();
    WiFiClient accept();
    operator bool();

    int fd() const { return sockfd_; }

private:
    uint16_t port_;
    int sockfd_;
};

#endif
