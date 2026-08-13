#include <Arduino.h>

#include "USB.h"
#include "USBCDC.h"
#include "USBHID.h"

extern "C" {
#include "board_gpio.h"
#ifdef BL616CL_USB_DEBUG_LOG
#include "bflb_uart.h"
#endif
}
#include "usbd_core.h"
#include "usbd_cdc_acm.h"
#include "usbd_hid.h"

#include <string.h>

#define BL616CL_USB_BUS_ID 0U
#define CDC_INT_EP 0x85
#define CDC_OUT_EP 0x04
#define CDC_IN_EP 0x83
#define HID_OUT_EP 0x02
#define HID_IN_EP 0x81

#ifdef CONFIG_USB_HS
#define CDC_MAX_MPS 512
#else
#define CDC_MAX_MPS 64
#endif

USBClass USB;
USBCDC *USBCDC::instance_ = nullptr;
__attribute__((weak)) USBHID HID;

static uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01,
                               0x2341, 0x1002, 0x0100, 0x01)
};

static uint8_t config_descriptor[9 + CDC_ACM_DESCRIPTOR_LEN + HID_CUSTOM_INOUT_DESCRIPTOR_LEN] = {
    USB_CONFIG_DESCRIPTOR_INIT(sizeof(config_descriptor), 0x03, 0x01,
                               USB_CONFIG_BUS_POWERED, 100),
    CDC_ACM_DESCRIPTOR_INIT(0x00, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP,
                            CDC_MAX_MPS, 0x00),
    HID_CUSTOM_INOUT_DESCRIPTOR_INIT(0x02, HID_SUBCLASS_NONE,
                                     CFG_TUD_HID_EP_BUFSIZE,
                                     HID_OUT_EP, HID_IN_EP,
                                     CFG_TUD_HID_EP_BUFSIZE, 10)
};

static const uint8_t device_qualifier_descriptor[] = {
    0x0A, 0x06, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x01, 0x00
};

static char manufacturer_string[64] = "Arduino";
static char product_string[64] = "BL616CL USB";
static char serial_string[32] = "0";
static const char language_string[] = {0x09, 0x04, 0x00};

static const uint8_t *device_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return device_descriptor;
}

static const uint8_t *config_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return config_descriptor;
}

static const uint8_t *device_qualifier_callback(uint8_t speed)
{
    (void)speed;
    return device_qualifier_descriptor;
}

static const char *string_descriptor_callback(uint8_t speed, uint8_t index)
{
    (void)speed;
    switch (index) {
        case 0:
            return language_string;
        case 1:
            return manufacturer_string;
        case 2:
            return product_string;
        case 3:
            return serial_string;
        default:
            return nullptr;
    }
}

static const struct usb_descriptor usb_descriptors = {
    device_descriptor_callback,
    config_descriptor_callback,
    device_qualifier_callback,
    nullptr,
    string_descriptor_callback,
    nullptr,
    nullptr,
    nullptr,
    nullptr
};

static struct usbd_interface cdc_interface0;
static struct usbd_interface cdc_interface1;
static struct usbd_interface hid_interface;

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t cdc_rx_buffer[4096];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t hid_rx_buffer[CFG_TUD_HID_EP_BUFSIZE];

#ifdef BL616CL_USB_DEBUG_LOG
static void usb_log(const char *message)
{
    struct bflb_device_s *uart = bflb_device_get_by_name("uart0");
    if (uart == NULL) {
        return;
    }
    for (const char *p = message; *p != '\0'; ++p) {
        bflb_uart_putchar(uart, *p);
    }
    bflb_uart_putchar(uart, '\r');
    bflb_uart_putchar(uart, '\n');
}
#else
#define usb_log(...) ((void)0)
#endif

static void cdc_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes);
static void cdc_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes);
static void hid_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes);
static void hid_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes);

static struct usbd_endpoint cdc_out_endpoint = {CDC_OUT_EP, cdc_out_callback};
static struct usbd_endpoint cdc_in_endpoint = {CDC_IN_EP, cdc_in_callback};
static struct usbd_endpoint hid_out_endpoint = {HID_OUT_EP, hid_out_callback};
static struct usbd_endpoint hid_in_endpoint = {HID_IN_EP, hid_in_callback};

static void cdc_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    if (USBCDC::instance() != nullptr) {
        USBCDC::instance()->onOutData(cdc_rx_buffer, nbytes);
    }
    usbd_ep_start_read(BL616CL_USB_BUS_ID, CDC_OUT_EP, cdc_rx_buffer,
                       sizeof(cdc_rx_buffer));
}

static void cdc_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    (void)nbytes;
    if (USBCDC::instance() != nullptr) {
        USBCDC::instance()->onTxComplete();
    }
}

static void hid_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    HID.onOutData(hid_rx_buffer, nbytes);
    usbd_ep_start_read(BL616CL_USB_BUS_ID, HID_OUT_EP, hid_rx_buffer,
                       sizeof(hid_rx_buffer));
}

static void hid_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    (void)nbytes;
    HID.onTxComplete();
}

static void usb_event_handler(uint8_t busid, uint8_t event)
{
    (void)busid;
    switch (event) {
        case USBD_EVENT_RESET:
            usb_log("usb:event_reset");
            if (USBCDC::instance() != nullptr) {
                USBCDC::instance()->end();
            }
            break;
        case USBD_EVENT_SUSPEND:
            usb_log("usb:event_suspend");
            if (USBCDC::instance() != nullptr) {
                USBCDC::instance()->end();
            }
            break;
        case USBD_EVENT_DISCONNECTED:
            usb_log("usb:event_disconnected");
            if (USBCDC::instance() != nullptr) {
                USBCDC::instance()->end();
            }
            break;
        case USBD_EVENT_CONFIGURED:
            usb_log("usb:event_configured");
            if (USBCDC::instance() != nullptr) {
                USBCDC::instance()->begin(USBCDC::instance()->baudRate());
            }
            usbd_ep_start_read(BL616CL_USB_BUS_ID, CDC_OUT_EP, cdc_rx_buffer,
                               sizeof(cdc_rx_buffer));
            if (HID.device() != nullptr) {
                usbd_ep_start_read(BL616CL_USB_BUS_ID, HID_OUT_EP, hid_rx_buffer,
                                   sizeof(hid_rx_buffer));
            }
            break;
        case USBD_EVENT_CONNECTED:
            usb_log("usb:event_connected");
            break;
        case USBD_EVENT_RESUME:
            usb_log("usb:event_resume");
            break;
        default:
            break;
    }
}

USBClass::USBClass()
    : vid_(0x2341), pid_(0x1002), firmware_version_(0x0100), initialized_(false)
{
}

void USBClass::manufacturerName(const char *name)
{
    if (name != nullptr) {
        strncpy(manufacturer_string, name, sizeof(manufacturer_string) - 1);
        manufacturer_string[sizeof(manufacturer_string) - 1] = '\0';
    }
}

void USBClass::productName(const char *name)
{
    if (name != nullptr) {
        strncpy(product_string, name, sizeof(product_string) - 1);
        product_string[sizeof(product_string) - 1] = '\0';
    }
}

void USBClass::begin()
{
    if (initialized_) {
        return;
    }

    usb_log("usb:begin");
    device_descriptor[8] = static_cast<uint8_t>(vid_ & 0xFFU);
    device_descriptor[9] = static_cast<uint8_t>((vid_ >> 8) & 0xFFU);
    device_descriptor[10] = static_cast<uint8_t>(pid_ & 0xFFU);
    device_descriptor[11] = static_cast<uint8_t>((pid_ >> 8) & 0xFFU);
    device_descriptor[12] = static_cast<uint8_t>(firmware_version_ & 0xFFU);
    device_descriptor[13] = static_cast<uint8_t>((firmware_version_ >> 8) & 0xFFU);

    board_usb_gpio_init();
    usb_log("usb:gpio_init_done");

    usbd_desc_register(BL616CL_USB_BUS_ID, &usb_descriptors);
    usb_log("usb:desc_registered");
    usbd_add_interface(BL616CL_USB_BUS_ID,
                       usbd_cdc_acm_init_intf(BL616CL_USB_BUS_ID, &cdc_interface0));
    usbd_add_interface(BL616CL_USB_BUS_ID,
                       usbd_cdc_acm_init_intf(BL616CL_USB_BUS_ID, &cdc_interface1));
    usb_log("usb:cdc_interfaces");

    uint16_t report_size = 0;
    const uint8_t *report = HID.reportDescriptor(report_size);
    if (report != nullptr && report_size != 0) {
        usbd_add_interface(BL616CL_USB_BUS_ID,
                           usbd_hid_init_intf(BL616CL_USB_BUS_ID, &hid_interface,
                                             report, report_size));
        usbd_add_endpoint(BL616CL_USB_BUS_ID, &hid_out_endpoint);
        usbd_add_endpoint(BL616CL_USB_BUS_ID, &hid_in_endpoint);
    }

    usbd_add_endpoint(BL616CL_USB_BUS_ID, &cdc_out_endpoint);
    usbd_add_endpoint(BL616CL_USB_BUS_ID, &cdc_in_endpoint);
    usb_log("usb:endpoints_registered");
    usbd_initialize(BL616CL_USB_BUS_ID, 0, usb_event_handler);
    usb_log("usb:initialized");
    initialized_ = true;
}

USBCDC::USBCDC(uint8_t itf)
    : interface_(itf), baud_(0), event_cb_(nullptr), active_(false),
      tx_busy_(false), rx_head_(0), rx_tail_(0), rx_count_(0)
{
    instance_ = this;
}

USBCDC::~USBCDC()
{
    if (instance_ == this) {
        instance_ = nullptr;
    }
}

void USBCDC::begin(unsigned long baud)
{
    baud_ = baud;
    active_ = true;
}

void USBCDC::end()
{
    active_ = false;
    tx_busy_ = false;
    rx_head_ = 0;
    rx_tail_ = 0;
    rx_count_ = 0;
}

int USBCDC::available()
{
    return static_cast<int>(rx_count_);
}

int USBCDC::read()
{
    if (rx_count_ == 0) {
        return -1;
    }
    uint8_t value = rx_buffer_[rx_tail_];
    rx_tail_ = (rx_tail_ + 1U) % sizeof(rx_buffer_);
    --rx_count_;
    return value;
}

int USBCDC::peek()
{
    return rx_count_ == 0 ? -1 : rx_buffer_[rx_tail_];
}

size_t USBCDC::write(uint8_t value)
{
    return write(&value, 1);
}

size_t USBCDC::write(const uint8_t *buffer, size_t size)
{
    if (!active_ || tx_busy_ || buffer == nullptr || size == 0 ||
        size > sizeof(tx_buffer_)) {
        return 0;
    }
    memcpy(tx_buffer_, buffer, size);
    tx_busy_ = true;
    if (usbd_ep_start_write(BL616CL_USB_BUS_ID, CDC_IN_EP, tx_buffer_, size) < 0) {
        tx_busy_ = false;
        return 0;
    }
    return size;
}

int USBCDC::availableForWrite()
{
    return (!active_ || tx_busy_) ? 0 : static_cast<int>(sizeof(tx_buffer_));
}

void USBCDC::flush()
{
}

void USBCDC::handleLineCoding(uint32_t baud)
{
    baud_ = baud;
    if (event_cb_ != nullptr) {
        arduino_usb_cdc_event_data_t data = {};
        data.line_coding.bit_rate = baud;
        event_cb_(nullptr, ARDUINO_USB_CDC_EVENTS,
                  ARDUINO_USB_CDC_LINE_CODING_EVENT, &data);
    }
}

void USBCDC::onOutData(const uint8_t *data, uint32_t size)
{
    if (data == nullptr || size == 0 || !active_) {
        return;
    }
    uint32_t capacity = sizeof(rx_buffer_);
    uint32_t free_space = capacity - rx_count_;
    if (size > free_space) {
        size = free_space;
    }
    for (uint32_t i = 0; i < size; ++i) {
        rx_buffer_[rx_head_] = data[i];
        rx_head_ = (rx_head_ + 1U) % capacity;
    }
    rx_count_ += size;
}

void USBCDC::onTxComplete()
{
    tx_busy_ = false;
}

USBHID::USBHID()
    : device_(nullptr), report_size_(0), report_descriptor_(nullptr),
      tx_busy_(false)
{
}

void USBHID::addDevice(USBHIDDevice *device, uint16_t report_size)
{
    device_ = device;
    report_size_ = report_size;
    report_descriptor_ = nullptr;
    if (device_ != nullptr) {
        static uint8_t report_storage[256];
        uint16_t len = device_->_onGetDescriptor(report_storage);
        if (len != 0) {
            report_descriptor_ = report_storage;
            report_size_ = len;
        }
    }
}

void USBHID::begin()
{
}

void USBHID::SendReport(uint8_t report_id, const uint8_t *buffer, uint16_t len,
                        uint32_t timeout_ms)
{
    (void)report_id;
    (void)timeout_ms;
    if (tx_busy_ || buffer == nullptr || len == 0 ||
        len > sizeof(tx_buffer_)) {
        return;
    }
    memcpy(tx_buffer_, buffer, len);
    tx_busy_ = true;
    if (usbd_ep_start_write(BL616CL_USB_BUS_ID, HID_IN_EP, tx_buffer_, len) < 0) {
        tx_busy_ = false;
    }
}

void USBHID::onOutData(const uint8_t *buffer, uint16_t len)
{
    if (device_ != nullptr && buffer != nullptr && len != 0) {
        device_->_onOutput(0, buffer, len);
    }
}

uint16_t USBHID::onGetFeature(uint8_t *buffer, uint16_t len)
{
    return device_ == nullptr ? 0 : device_->_onGetFeature(0, buffer, len);
}

void USBHID::onTxComplete()
{
    tx_busy_ = false;
}

extern "C" void usbd_cdc_acm_set_line_coding(uint8_t busid, uint8_t intf,
                                             struct cdc_line_coding *line_coding)
{
    (void)busid;
    (void)intf;
    if (USBCDC::instance() != nullptr && line_coding != nullptr) {
        USBCDC::instance()->handleLineCoding(line_coding->dwDTERate);
    }
}

extern "C" void usbd_cdc_acm_get_line_coding(uint8_t busid, uint8_t intf,
                                             struct cdc_line_coding *line_coding)
{
    (void)busid;
    (void)intf;
    if (line_coding != nullptr) {
        line_coding->dwDTERate = USBCDC::instance() == nullptr
                                     ? 115200
                                     : USBCDC::instance()->baudRate();
        line_coding->bCharFormat = 0;
        line_coding->bParityType = 0;
        line_coding->bDataBits = 8;
    }
}

extern "C" void usbd_cdc_acm_set_dtr(uint8_t busid, uint8_t intf, bool dtr)
{
    (void)busid;
    (void)intf;
    (void)dtr;
}

extern "C" void usbd_cdc_acm_set_rts(uint8_t busid, uint8_t intf, bool rts)
{
    (void)busid;
    (void)intf;
    (void)rts;
}

extern "C" void usbd_cdc_acm_send_break(uint8_t busid, uint8_t intf)
{
    (void)busid;
    (void)intf;
}

extern "C" void usbd_hid_get_report(uint8_t busid, uint8_t intf,
                                    uint8_t report_id, uint8_t report_type,
                                    uint8_t **data, uint32_t *len)
{
    (void)busid;
    (void)intf;
    (void)report_id;
    (void)report_type;
    if (data == nullptr || len == nullptr) {
        return;
    }
    *len = HID.onGetFeature(*data, 64);
}

extern "C" uint8_t usbd_hid_get_idle(uint8_t busid, uint8_t intf,
                                     uint8_t report_id)
{
    (void)busid;
    (void)intf;
    (void)report_id;
    return 0;
}

extern "C" uint8_t usbd_hid_get_protocol(uint8_t busid, uint8_t intf)
{
    (void)busid;
    (void)intf;
    return 0;
}

extern "C" void usbd_hid_set_report(uint8_t busid, uint8_t intf,
                                    uint8_t report_id, uint8_t report_type,
                                    uint8_t *report, uint32_t report_len)
{
    (void)busid;
    (void)intf;
    (void)report_id;
    (void)report_type;
    if (HID.device() != nullptr && report != nullptr) {
        HID.device()->_onSetFeature(report_id, report, report_len);
    }
}

extern "C" void usbd_hid_set_idle(uint8_t busid, uint8_t intf,
                                  uint8_t report_id, uint8_t duration)
{
    (void)busid;
    (void)intf;
    (void)report_id;
    (void)duration;
}

extern "C" void usbd_hid_set_protocol(uint8_t busid, uint8_t intf,
                                      uint8_t protocol)
{
    (void)busid;
    (void)intf;
    (void)protocol;
}
