#ifndef BL616CL_ESP32_COMPAT_USBCDC_H_
#define BL616CL_ESP32_COMPAT_USBCDC_H_

#include <Arduino.h>
#include <Stream.h>
#include "esp32-hal-tinyusb.h"

typedef void (*USBCDC_EventCallback)(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data);

class USBCDC : public Stream {
public:
    explicit USBCDC(uint8_t itf = 0) : interface_(itf), baud_(0), event_cb_(nullptr) {}
    virtual ~USBCDC() {}

    void begin(unsigned long baud = 0) { baud_ = baud; }
    void end() {}
    unsigned long baudRate() { return baud_; }
    void onEvent(USBCDC_EventCallback callback) { event_cb_ = callback; }
    void enableReboot(bool enable) { (void)enable; }
    void setRxBufferSize(size_t size) { (void)size; }
    void setTxBufferSize(size_t size) { (void)size; }

    virtual int available() override { return 0; }
    virtual int read() override { return -1; }
    virtual int peek() override { return -1; }
    virtual size_t write(uint8_t value) override { (void)value; return 0; }
    virtual size_t write(const uint8_t *buffer, size_t size) override { (void)buffer; (void)size; return 0; }
    virtual int availableForWrite() override { return 0; }
    virtual void flush() override {}
    operator bool() const { return false; }

private:
    uint8_t interface_;
    unsigned long baud_;
    USBCDC_EventCallback event_cb_;
};

#endif
