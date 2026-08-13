#ifndef BL616CL_ESP32_COMPAT_ARDUINO_DEBUG_UTILS_H_
#define BL616CL_ESP32_COMPAT_ARDUINO_DEBUG_UTILS_H_

#include <Arduino.h>

enum DebugLevel {
    DBG_NONE = 0,
    DBG_ERROR = 1,
    DBG_WARNING = 2,
    DBG_INFO = 3,
    DBG_DEBUG = 4,
    DBG_VERBOSE = 5
};

class Arduino_DebugUtils {
public:
    void setDebugOutputStream(Stream *stream) { (void)stream; }
    void setDebugLevel(int level) { (void)level; }
    void newlineOn() {}
    void newlineOff() {}
};

extern Arduino_DebugUtils Debug;

#endif
