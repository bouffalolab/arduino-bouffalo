#pragma once

#include <bouffalo_sdk.h>


class Wire    {
private:
    struct bflb_device_s *i2c0;
    uint8_t rbuf[128];
    int available_count;
    int index;
    int wire_timeout;
    bool wire_timeout_flag;
public: 

    bool getWireTimeoutFlag();
    bool clearWireTimeoutFlag();
    void setWireTimeout(int timeout, bool reset_on_timeout);
    void onRequest(void (*callback)());
    void onReceive(void (*callback)(int));
    void setClock(int clockFrequency);
    int read();
    int available();
    int write(uint8_t *str, int len);
    int write(uint8_t *str);
    int write(unsigned char value);
    void endTransmission(bool stop);
    void endTransmission();
    void beginTransmission(unsigned char addr);
    int requestFrom(unsigned char addr, int quantity, bool stop);
    int requestFrom(unsigned char addr, int quantity);
    void end();
    void begin(unsigned char addr);
    void begin();
   
};





