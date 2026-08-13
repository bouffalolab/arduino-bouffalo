#ifndef BL616CL_ESP32_COMPAT_UPDATE_H_
#define BL616CL_ESP32_COMPAT_UPDATE_H_

#include <Arduino.h>

class UpdateClass {
public:
    bool begin(size_t size) { (void)size; return false; }
    size_t write(uint8_t value) { (void)value; return 0; }
    size_t write(const uint8_t *buffer, size_t size) { (void)buffer; (void)size; return 0; }
    bool end(bool evenIfRemaining = false) { (void)evenIfRemaining; return false; }
    bool isRunning() { return false; }
    bool hasError() { return true; }
    void abort() {}
};

extern UpdateClass Update;

#endif
