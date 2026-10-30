/* Raw HID transport using CherryUSB (Apache-2.0). Application owns reports. */
#if defined(USB_VID)
#include <stddef.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "usbd_core.h"
#include "usbd_hid.h"
#include "usb_hid_port.h"

#define HID_IN_EP 0x81
#define HID_OUT_EP 0x02
#define HID_INTERFACE 2

static const struct usb_hid_port_config *config;
static struct usbd_interface hid_interface;
static uint8_t descriptor[] = {
    HID_CUSTOM_INOUT_DESCRIPTOR_INIT(HID_INTERFACE, 0, 0, HID_OUT_EP, HID_IN_EP,
                                     USB_HID_PORT_PACKET_SIZE, 1)
};
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t rx_packet[USB_HID_PORT_PACKET_SIZE];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t tx_packet[USB_HID_PORT_PACKET_SIZE];
static volatile uint32_t rx_size;
static volatile bool configured;
static volatile bool tx_busy;

/**
 * @brief Publish a received HID packet for task-context consumption.
 */
static void hid_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    rx_size = nbytes; /* Leave OUT unarmed until the reader has copied it. */
}

/**
 * @brief Release the HID transmit buffer after endpoint completion.
 */
static void hid_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    (void)nbytes;
    tx_busy = false;
}

static struct usbd_endpoint out_ep = { .ep_addr = HID_OUT_EP, .ep_cb = hid_out };
static struct usbd_endpoint in_ep = { .ep_addr = HID_IN_EP, .ep_cb = hid_in };

/**
 * @brief Register an application-owned HID descriptor and Feature callbacks before USB
 * startup.
 */
void usb_hid_port_register(const struct usb_hid_port_config *cfg)
{
    config = cfg;
}

/**
 * @brief Initialize the optional HID interface and return its configuration descriptor
 * contribution.
 */
const uint8_t *usb_hid_port_init(void)
{
    if (config == NULL) {
        return NULL;
    }
    descriptor[16] = config->report_descriptor_size;
    descriptor[17] = config->report_descriptor_size >> 8;
    usbd_add_interface(0, usbd_hid_init_intf(0, &hid_interface, config->report_descriptor,
                                           config->report_descriptor_size));
    usbd_add_endpoint(0, &out_ep);
    usbd_add_endpoint(0, &in_ep);
    return descriptor;
}

/**
 * @brief Update optional HID endpoint state on USB lifecycle events.
 */
void usb_hid_port_event(uint8_t event)
{
    if (config == NULL) {
        return;
    }
    if (event == USBD_EVENT_CONFIGURED) {
        configured = true;
        rx_size = 0;
        tx_busy = false;
        usbd_ep_start_read(0, HID_OUT_EP, rx_packet, sizeof(rx_packet));
    } else if (event == USBD_EVENT_RESET || event == USBD_EVENT_DISCONNECTED) {
        configured = false;
        rx_size = 0;
        tx_busy = false;
    }
}

/**
 * @brief Copy one pending HID packet to the caller, or report that none is available.
 */
int usb_hid_port_read(uint8_t data[USB_HID_PORT_PACKET_SIZE])
{
    taskENTER_CRITICAL();
    int size = rx_size;
    if (size > 0) {
        memcpy(data, rx_packet, size);
        rx_size = 0;
        usbd_ep_start_read(0, HID_OUT_EP, rx_packet, sizeof(rx_packet));
    }
    taskEXIT_CRITICAL();
    return size;
}

/**
 * @brief Submit one fixed-size HID packet when the endpoint is ready.
 */
bool usb_hid_port_write(const uint8_t data[USB_HID_PORT_PACKET_SIZE])
{
    TickType_t start = xTaskGetTickCount();
    while (configured && tx_busy) {
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(100)) {
            return false;
        }
        vTaskDelay(1);
    }
    taskENTER_CRITICAL();
    bool ok = configured;
    if (ok) {
        memcpy(tx_packet, data, sizeof(tx_packet));
        tx_busy = true;
        ok = usbd_ep_start_write(0, HID_IN_EP, tx_packet, sizeof(tx_packet)) == 0;
        if (!ok) {
            tx_busy = false;
        }
    }
    taskEXIT_CRITICAL();
    return ok;
}

/**
 * @brief Provide a Feature response through the registered application callback.
 */
void usbd_hid_get_report(uint8_t busid, uint8_t intf, uint8_t report_id,
                         uint8_t report_type, uint8_t **data, uint32_t *len)
{
    (void)busid;
    *len = 0;
    if (intf == HID_INTERFACE && report_id == 0) {
        if (report_type == HID_REPORT_FEATURE && config->get_feature) {
            *len = config->get_feature(*data);
        } else if (report_type == HID_REPORT_INPUT) {
            memcpy(*data, tx_packet, sizeof(tx_packet));
            *len = sizeof(tx_packet);
        }
    }
}

/**
 * @brief Deliver supported Feature writes to the registered application callback.
 */
void usbd_hid_set_report(uint8_t busid, uint8_t intf, uint8_t report_id,
                         uint8_t report_type, uint8_t *report, uint32_t report_len)
{
    (void)busid;
    if (intf == HID_INTERFACE && report_id == 0 && report_type == HID_REPORT_FEATURE &&
        config->set_feature) {
        config->set_feature(report, report_len);
    }
}
#endif
