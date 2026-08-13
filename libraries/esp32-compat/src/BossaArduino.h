#ifndef BL616CL_ESP32_COMPAT_BOSSAARDUINO_H_
#define BL616CL_ESP32_COMPAT_BOSSAARDUINO_H_

#include <Arduino.h>

class FlasherObserver {
public:
    virtual ~FlasherObserver() {}
    virtual void onStatus(const char *message, ...) { (void)message; }
    virtual void onProgress(int num, int div) { (void)num; (void)div; }
};

class BossaArduino {
public:
    explicit BossaArduino(FlasherObserver &observer) : observer_(observer) {}
    virtual ~BossaArduino() {}

    bool connect(HardwareSerial &serial) { (void)serial; return false; }
    int flash(const char *filePath) { (void)filePath; return 0; }

protected:
    FlasherObserver &observer_;
};

#endif
