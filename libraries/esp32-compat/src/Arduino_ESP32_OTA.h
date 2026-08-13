#ifndef BL616CL_ESP32_COMPAT_ARDUINO_ESP32_OTA_H_
#define BL616CL_ESP32_COMPAT_ARDUINO_ESP32_OTA_H_

#include <Arduino.h>

class Arduino_ESP32_OTA {
public:
    enum class Error : int {
        None = 0,
        OtaStorageInit = -1,
        OtaDownloadFailed = -2,
        OtaVerifyFailed = -3,
        OtaUpdateFailed = -4
    };

    virtual ~Arduino_ESP32_OTA() {}

    void setMagic(uint32_t magic) { (void)magic; }
    void setCACert(const char *cert) { (void)cert; }
    void reset() {}

    virtual Error begin() { return Error::OtaStorageInit; }
    virtual Error begin(const char *filePath, uint32_t magic, bool formatOnFail = false)
    {
        (void)filePath; (void)magic; (void)formatOnFail;
        return Error::OtaStorageInit;
    }

    virtual int download(const char *url) { (void)url; return -1; }
    virtual int download(const char *url, const char *filePath) { (void)url; (void)filePath; return -1; }
    virtual int startDownload(const char *url) { (void)url; return -1; }
    virtual int startDownload(const char *url, const char *filePath) { (void)url; (void)filePath; return -1; }
    virtual int downloadProgress() { return 100; }
    virtual int downloadPoll() { return 0; }
    virtual void write_byte_to_flash(uint8_t data) { (void)data; }

    virtual Error verify() { return Error::None; }
    virtual Error update() { return Error::None; }
    virtual int update(const char *filePath) { (void)filePath; return 0; }
};

#endif
