#ifndef BL616CL_ESP32_COMPAT_USBHID_H_
#define BL616CL_ESP32_COMPAT_USBHID_H_

#include <Arduino.h>

class USBClass;

#define CFG_TUD_HID_EP_BUFSIZE 64

#define HID_USAGE_PAGE_VENDOR 0x06
#define HID_COLLECTION_APPLICATION 0x01
#define HID_DATA 0x00
#define HID_VARIABLE 0x02
#define HID_ABSOLUTE 0x00

#define HID_USAGE_PAGE_N(page, size) 0x06, 0x00, 0xFF
#define HID_USAGE(value) 0x09, value
#define HID_COLLECTION(value) 0xA1, value
#define HID_COLLECTION_END 0xC0
#define HID_LOGICAL_MIN(value) 0x15, value
#define HID_LOGICAL_MAX_N(value, size) 0x27, (value) & 0xFF, ((value) >> 8) & 0xFF, ((value) >> 16) & 0xFF, ((value) >> 24) & 0xFF
#define HID_REPORT_SIZE(value) 0x75, value
#define HID_REPORT_COUNT(value) 0x95, value
#define HID_INPUT(value) 0x81, value
#define HID_OUTPUT(value) 0x91, value
#define HID_FEATURE(value) 0xB1, value

class USBHIDDevice {
public:
    virtual ~USBHIDDevice() {}
    virtual uint16_t _onGetDescriptor(uint8_t *buffer) { (void)buffer; return 0; }
    virtual void _onOutput(uint8_t report_id, const uint8_t *buffer, uint16_t len) { (void)report_id; (void)buffer; (void)len; }
    virtual void _onSetFeature(uint8_t report_id, const uint8_t *buffer, uint16_t len) { (void)report_id; (void)buffer; (void)len; }
    virtual uint16_t _onGetFeature(uint8_t report_id, uint8_t *buffer, uint16_t len) { (void)report_id; (void)buffer; (void)len; return 0; }
};

class USBHID {
public:
    USBHID();

    void addDevice(USBHIDDevice *device, uint16_t report_size);
    void begin();
    void SendReport(uint8_t report_id, const uint8_t *buffer, uint16_t len,
                    uint32_t timeout_ms = 100);

    USBHIDDevice *device() const { return device_; }
    const uint8_t *reportDescriptor(uint16_t &size) const { size = report_size_; return report_descriptor_; }
    void onOutData(const uint8_t *buffer, uint16_t len);
    uint16_t onGetFeature(uint8_t *buffer, uint16_t len);
    void onTxComplete();

private:
    USBHIDDevice *device_;
    uint16_t report_size_;
    const uint8_t *report_descriptor_;
    uint8_t tx_buffer_[CFG_TUD_HID_EP_BUFSIZE];
    bool tx_busy_;

    friend class USBClass;
};

extern USBHID HID;

#endif
