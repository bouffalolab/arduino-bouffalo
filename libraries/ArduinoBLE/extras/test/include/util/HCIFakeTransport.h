//#include "Common.h"
#pragma once

#include "HCITransport.h"
#include <functional>

class HCIFakeTransportClass : public HCITransportInterface
{
public:
    HCIFakeTransportClass() {};
    ~HCIFakeTransportClass() {};

    int begin() {return 0;}
    void end() {return;}
    void wait(unsigned long timeout) {return;}
    std::function<int()> availableHook;
    std::function<int()> readHook;
    int available() {return availableHook ? availableHook() : 0;}
    int peek() {return 0;}
    int read() {return readHook ? readHook() : 0;}
    size_t write(const uint8_t* data, size_t length) {return 0;}
};

extern HCIFakeTransportClass HCIFakeTransport;
