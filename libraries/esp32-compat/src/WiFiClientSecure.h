#ifndef BL616CL_ESP32_COMPAT_WIFICLIENTSECURE_H_
#define BL616CL_ESP32_COMPAT_WIFICLIENTSECURE_H_

#include "WiFiClient.h"

class WiFiClientSecure : public WiFiClient {
public:
    WiFiClientSecure() {}
    virtual ~WiFiClientSecure() {}

    void setCACert(const char *cert) { (void)cert; }
    void setCACertBundle(const uint8_t *bundle) { (void)bundle; }
    void setCertificate(const char *cert) { (void)cert; }
    void setPrivateKey(const char *key) { (void)key; }
    void setInsecure() {}
};

#endif
