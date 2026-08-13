#ifndef BL616CL_ESP32_COMPAT_USB_H_
#define BL616CL_ESP32_COMPAT_USB_H_

#include <Arduino.h>

class USBClass {
public:
    void VID(uint16_t vid) { (void)vid; }
    void PID(uint16_t pid) { (void)pid; }
    void manufacturerName(const char *name) { (void)name; }
    void productName(const char *name) { (void)name; }
    void firmwareVersion(uint16_t version) { (void)version; }
    void begin() {}
    void enableDFU() {}
};

extern USBClass USB;

#endif
