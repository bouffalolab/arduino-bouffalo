#ifndef BL616CL_ESP32_COMPAT_USBCDC_H_
#define BL616CL_ESP32_COMPAT_USBCDC_H_

#include <Arduino.h>
#include <Stream.h>
#include "esp32-hal-tinyusb.h"

class USBClass;

typedef void (*USBCDC_EventCallback)(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data);

class USBCDC : public Stream {
public:
    explicit USBCDC(uint8_t itf = 0);
    virtual ~USBCDC();
    static USBCDC *instance() { return instance_; }

    void begin(unsigned long baud = 0);
    void end();
    unsigned long baudRate() const { return baud_; }
    void onEvent(USBCDC_EventCallback callback) { event_cb_ = callback; }
    void enableReboot(bool enable) { (void)enable; }
    void setRxBufferSize(size_t size) { (void)size; }
    void setTxBufferSize(size_t size) { (void)size; }

    virtual int available() override;
    virtual int read() override;
    virtual int peek() override;
    virtual size_t write(uint8_t value) override;
    virtual size_t write(const uint8_t *buffer, size_t size) override;
    virtual int availableForWrite() override;
    virtual void flush() override;
    operator bool() const { return active_; }

    void handleLineCoding(uint32_t baud);
    void onOutData(const uint8_t *data, uint32_t size);
    void onTxComplete();

private:
    void armOut();
    static USBCDC *instance_;

    uint8_t interface_;
    unsigned long baud_;
    USBCDC_EventCallback event_cb_;
    bool active_;
    bool tx_busy_;
    uint8_t rx_buffer_[4096];
    uint32_t rx_head_;
    uint32_t rx_tail_;
    uint32_t rx_count_;
    /* Staged USB IN data.  Allocated in non-cacheable RAM so the controller
     * DMA engine can read it directly (see esp32_usb.cpp). */
    static uint8_t tx_buffer_[4096];

    friend class USBClass;
};

#endif
