#ifndef BL616CL_ESP32_COMPAT_WIFIGENERIC_H_
#define BL616CL_ESP32_COMPAT_WIFIGENERIC_H_

#include <Arduino.h>
#include <IPAddress.h>

class WiFiGenericClass {
public:
    static int hostByName(const char *hostname, IPAddress &address);
};

#endif
