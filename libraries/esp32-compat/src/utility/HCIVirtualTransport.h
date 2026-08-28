#ifndef BL616CL_ESP32_COMPAT_HCIVIRTUALTRANSPORT_H_
#define BL616CL_ESP32_COMPAT_HCIVIRTUALTRANSPORT_H_

#include <Arduino.h>

// ArduinoBLE host (running on the RA4M1) talks HCI to the BL616CL through the
// bridge's AT+HCI_* commands.  This class is the firmware-side end of that
// virtual transport: HCI bytes written by the host are forwarded into the
// BL616CL BLE controller (host -> controller), and controller events/ACL data
// are queued here for the host to poll with available()/read().

class HCIVirtualTransportClass {
public:
    bool begin();
    void end();
    void wait(int timeoutMs);
    int available();
    int read();
    size_t write(const uint8_t *buffer, size_t size);
};

extern HCIVirtualTransportClass HCIVirtualTransport;

#endif
