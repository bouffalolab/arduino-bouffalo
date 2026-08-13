#ifndef BL616CL_ESP32_COMPAT_HCIVIRTUALTRANSPORT_H_
#define BL616CL_ESP32_COMPAT_HCIVIRTUALTRANSPORT_H_

#include <Arduino.h>

class HCIVirtualTransportClass {
public:
    bool begin() { return false; }
    void end() {}
    void wait(int timeoutMs) { (void)timeoutMs; }
    int available() { return 0; }
    int read() { return -1; }
    size_t write(const uint8_t *buffer, size_t size) { (void)buffer; (void)size; return 0; }
};

extern HCIVirtualTransportClass HCIVirtualTransport;

#endif
