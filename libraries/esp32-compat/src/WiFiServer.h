#ifndef BL616CL_ESP32_COMPAT_WIFISERVER_H_
#define BL616CL_ESP32_COMPAT_WIFISERVER_H_

#include <Arduino.h>
#include <Server.h>
#include "WiFiClient.h"

class WiFiServer : public Server {
public:
    WiFiServer() : port_(0) {}
    explicit WiFiServer(uint16_t port) : port_(port) {}
    virtual ~WiFiServer() {}

    void begin() override {}
    void begin(uint16_t port) { (void)port; }
    void end() {}
    WiFiClient available() { return WiFiClient(); }
    operator bool() { return false; }

private:
    uint16_t port_;
};

#endif
