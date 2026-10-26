/* Optional 64-byte raw HID interface, alongside Serial's CDC interfaces. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USB_HID_PORT_PACKET_SIZE 64

struct usb_hid_port_config {
    const uint8_t *report_descriptor;
    uint16_t report_descriptor_size;
    /* USB ISR callbacks: copy data or set a flag; never block here. */
    uint16_t (*get_feature)(uint8_t *data);
    void (*set_feature)(const uint8_t *data, uint16_t size);
};

/* Register once, before Core starts USB (e.g. from a global constructor).
 * config and its descriptor must remain valid for the lifetime of USB.
 * Interface 2, no report IDs, 64-byte input/output/feature reports.
 */
void usb_hid_port_register(const struct usb_hid_port_config *config);
/* Task context, one reader/writer. read returns 0 when no packet is ready.
 * write queues 64 bytes; false on disconnect, driver error, or 100 ms timeout.
 */
int usb_hid_port_read(uint8_t data[USB_HID_PORT_PACKET_SIZE]);
bool usb_hid_port_write(const uint8_t data[USB_HID_PORT_PACKET_SIZE]);

/* Core USB integration; returns the 32-byte interface descriptor, or NULL. */
const uint8_t *usb_hid_port_init(void);
void usb_hid_port_event(uint8_t event);

#ifdef __cplusplus
}
#endif
