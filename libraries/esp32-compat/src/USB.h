#ifndef BL616CL_ESP32_COMPAT_USB_H_
#define BL616CL_ESP32_COMPAT_USB_H_

#include <Arduino.h>

class USBClass {
public:
    USBClass();

    void VID(uint16_t vid) { vid_ = vid; }
    void PID(uint16_t pid) { pid_ = pid; }
    void manufacturerName(const char *name);
    void productName(const char *name);
    void firmwareVersion(uint16_t version) { firmware_version_ = version; }
    void begin();
    void enableDFU() {}

    bool initialized() const { return initialized_; }
    uint16_t VID() const { return vid_; }
    uint16_t PID() const { return pid_; }

private:
    uint16_t vid_;
    uint16_t pid_;
    uint16_t firmware_version_;
    bool initialized_;
};

extern USBClass USB;

#endif
