#include <Arduino.h>

extern "C" {
#include "usbd_cdc_acm.h"
#include "usbd_core.h"
#include "usbd_hid.h"
}

namespace {

constexpr uint8_t kUsbBusId = 0U;
constexpr uint16_t kVendorId = 0x2341U;
constexpr uint16_t kProductId = 0x1002U;
constexpr uint16_t kPacketSize = 64U;

constexpr uint8_t kCdcInterruptEndpoint = 0x81U;
constexpr uint8_t kCdcOutEndpoint = 0x02U;
constexpr uint8_t kCdcInEndpoint = 0x83U;
constexpr uint8_t kHidOutEndpoint = 0x04U;
constexpr uint8_t kHidInEndpoint = 0x85U;

constexpr size_t kCdcDescriptorLength = CDC_ACM_DESCRIPTOR_LEN;
constexpr size_t kHidDescriptorLength = HID_CUSTOM_INOUT_DESCRIPTOR_LEN;
constexpr size_t kConfigurationDescriptorLength =
    9U + kCdcDescriptorLength + kHidDescriptorLength;

static const uint8_t kHidReportDescriptor[] = {
    0x06, 0x00, 0xFF, // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,       // Usage (0x01)
    0xA1, 0x01,       // Collection (Application)
    0x09, 0x02,       //   Usage (Input)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x40,       //   Report Count (64)
    0x81, 0x02,       //   Input (Data, Variable, Absolute)
    0x09, 0x03,       //   Usage (Output)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x40,       //   Report Count (64)
    0x91, 0x02,       //   Output (Data, Variable, Absolute)
    0x09, 0x04,       //   Usage (Feature)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x40,       //   Report Count (64)
    0xB1, 0x02,       //   Feature (Data, Variable, Absolute)
    0xC0,             // End Collection
};

static const uint8_t kDeviceDescriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(
        USB_2_0,
        0xEF,
        0x02,
        0x01,
        kVendorId,
        kProductId,
        0x0100,
        0x01),
};

static const uint8_t kConfigurationDescriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(
        kConfigurationDescriptorLength,
        0x03,
        0x01,
        USB_CONFIG_BUS_POWERED,
        100U),
    CDC_ACM_DESCRIPTOR_INIT(
        0x00,
        kCdcInterruptEndpoint,
        kCdcOutEndpoint,
        kCdcInEndpoint,
        kPacketSize,
        USB_STRING_PRODUCT_INDEX),
    HID_CUSTOM_INOUT_DESCRIPTOR_INIT(
        0x02,
        HID_SUBCLASS_NONE,
        sizeof(kHidReportDescriptor),
        kHidOutEndpoint,
        kHidInEndpoint,
        kPacketSize,
        0x01),
};

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t cdc_rx[kPacketSize];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t cdc_tx[kPacketSize];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t hid_rx[kPacketSize];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t hid_tx[kPacketSize];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t hid_feature[kPacketSize] = {
    0x42, 0x4C, 0x36, 0x31, 0x36, 0x43, 0x4C, 0x2D,
    0x55, 0x53, 0x42, 0x2D, 0x53, 0x4D, 0x4F, 0x4B,
    0x45,
};

volatile bool cdc_tx_busy = false;
volatile bool hid_tx_busy = false;
volatile bool usb_configured = false;
volatile uint32_t cdc_echo_count = 0U;
volatile uint32_t hid_echo_count = 0U;

struct usbd_interface cdc_control_interface;
struct usbd_interface cdc_data_interface;
struct usbd_interface hid_interface;

const uint8_t *device_descriptor_callback(uint8_t)
{
    return kDeviceDescriptor;
}

const uint8_t *configuration_descriptor_callback(uint8_t)
{
    return kConfigurationDescriptor;
}

const char *string_descriptor_callback(uint8_t, uint8_t index)
{
    static const char kLanguageId[] = {0x09, 0x04};

    switch (index) {
    case USB_STRING_LANGID_INDEX:
        return kLanguageId;
    case USB_STRING_MFC_INDEX:
        return "Arduino";
    case USB_STRING_PRODUCT_INDEX:
        return "UNO WiFi R4 CMSIS-DAP";
    case USB_STRING_SERIAL_INDEX:
        return "BL616CL-USB-SMOKE";
    default:
        return nullptr;
    }
}

const struct usb_descriptor kDescriptors = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = configuration_descriptor_callback,
    .device_quality_descriptor_callback = nullptr,
    .other_speed_descriptor_callback = nullptr,
    .string_descriptor_callback = string_descriptor_callback,
    .msosv1_descriptor = nullptr,
    .msosv2_descriptor = nullptr,
    .webusb_url_descriptor = nullptr,
    .bos_descriptor = nullptr,
};

void rearm_cdc_read()
{
    (void)usbd_ep_start_read(kUsbBusId, kCdcOutEndpoint, cdc_rx, sizeof(cdc_rx));
}

void rearm_hid_read()
{
    (void)usbd_ep_start_read(kUsbBusId, kHidOutEndpoint, hid_rx, sizeof(hid_rx));
}

void cdc_out_callback(uint8_t, uint8_t, uint32_t nbytes)
{
    if (nbytes > sizeof(cdc_rx)) {
        nbytes = sizeof(cdc_rx);
    }

    ++cdc_echo_count;
    if (!cdc_tx_busy && nbytes > 0U) {
        memcpy(cdc_tx, cdc_rx, nbytes);
        cdc_tx_busy = usbd_ep_start_write(kUsbBusId, kCdcInEndpoint, cdc_tx, nbytes) == 0;
    }
    rearm_cdc_read();
}

void cdc_in_callback(uint8_t, uint8_t, uint32_t)
{
    cdc_tx_busy = false;
}

void hid_out_callback(uint8_t, uint8_t, uint32_t nbytes)
{
    if (nbytes > sizeof(hid_rx)) {
        nbytes = sizeof(hid_rx);
    }

    ++hid_echo_count;
    if (!hid_tx_busy && nbytes == sizeof(hid_rx)) {
        memcpy(hid_tx, hid_rx, sizeof(hid_tx));
        hid_tx_busy = usbd_ep_start_write(kUsbBusId, kHidInEndpoint, hid_tx, sizeof(hid_tx)) == 0;
    }
    rearm_hid_read();
}

void hid_in_callback(uint8_t, uint8_t, uint32_t)
{
    hid_tx_busy = false;
}

struct usbd_endpoint cdc_out_endpoint = {
    .ep_addr = kCdcOutEndpoint,
    .ep_cb = cdc_out_callback,
};

struct usbd_endpoint cdc_in_endpoint = {
    .ep_addr = kCdcInEndpoint,
    .ep_cb = cdc_in_callback,
};

struct usbd_endpoint hid_out_endpoint = {
    .ep_addr = kHidOutEndpoint,
    .ep_cb = hid_out_callback,
};

struct usbd_endpoint hid_in_endpoint = {
    .ep_addr = kHidInEndpoint,
    .ep_cb = hid_in_callback,
};

void usb_event_callback(uint8_t, uint8_t event)
{
    switch (event) {
    case USBD_EVENT_RESET:
    case USBD_EVENT_DISCONNECTED:
        usb_configured = false;
        cdc_tx_busy = false;
        hid_tx_busy = false;
        break;
    case USBD_EVENT_CONFIGURED:
        usb_configured = true;
        rearm_cdc_read();
        rearm_hid_read();
        Serial.println("BL616CL_USB_SMOKE:CONFIGURED");
        break;
    default:
        break;
    }
}

void initialize_usb()
{
    usbd_desc_register(kUsbBusId, &kDescriptors);
    usbd_add_interface(
        kUsbBusId,
        usbd_cdc_acm_init_intf(kUsbBusId, &cdc_control_interface));
    usbd_add_interface(
        kUsbBusId,
        usbd_cdc_acm_init_intf(kUsbBusId, &cdc_data_interface));
    usbd_add_interface(
        kUsbBusId,
        usbd_hid_init_intf(
            kUsbBusId,
            &hid_interface,
            kHidReportDescriptor,
            sizeof(kHidReportDescriptor)));
    usbd_add_endpoint(kUsbBusId, &cdc_out_endpoint);
    usbd_add_endpoint(kUsbBusId, &cdc_in_endpoint);
    usbd_add_endpoint(kUsbBusId, &hid_out_endpoint);
    usbd_add_endpoint(kUsbBusId, &hid_in_endpoint);
    usbd_initialize(kUsbBusId, 0U, usb_event_callback);
}

} // namespace

extern "C" void usbd_hid_get_report(
    uint8_t,
    uint8_t,
    uint8_t,
    uint8_t,
    uint8_t **data,
    uint32_t *length)
{
    *data = hid_feature;
    *length = sizeof(hid_feature);
}

extern "C" void usbd_hid_set_report(
    uint8_t,
    uint8_t,
    uint8_t,
    uint8_t report_type,
    uint8_t *report,
    uint32_t report_length)
{
    if (report_type != HID_REPORT_FEATURE || report == nullptr) {
        return;
    }

    if (report_length > sizeof(hid_feature)) {
        report_length = sizeof(hid_feature);
    }
    memcpy(hid_feature, report, report_length);
    if (report_length < sizeof(hid_feature)) {
        memset(hid_feature + report_length, 0, sizeof(hid_feature) - report_length);
    }
    Serial.println("BL616CL_USB_SMOKE:FEATURE_SET");
}

void setup()
{
    Serial.begin(2000000);
    delay(25);
    Serial.println("BL616CL_USB_SMOKE:BOOT");
    initialize_usb();
    Serial.println("BL616CL_USB_SMOKE:STACK_READY vid=2341 pid=1002");
}

void loop()
{
    static uint32_t last_cdc_echo_count = 0U;
    static uint32_t last_hid_echo_count = 0U;

    if (cdc_echo_count != last_cdc_echo_count) {
        last_cdc_echo_count = cdc_echo_count;
        Serial.print("BL616CL_USB_SMOKE:CDC_ECHO count=");
        Serial.println(last_cdc_echo_count);
    }
    if (hid_echo_count != last_hid_echo_count) {
        last_hid_echo_count = hid_echo_count;
        Serial.print("BL616CL_USB_SMOKE:HID_ECHO count=");
        Serial.println(last_hid_echo_count);
    }
    delay(10);
}
