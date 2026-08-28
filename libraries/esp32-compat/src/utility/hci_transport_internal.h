#ifndef BL616CL_ESP32_COMPAT_HCI_TRANSPORT_INTERNAL_H_
#define BL616CL_ESP32_COMPAT_HCI_TRANSPORT_INTERNAL_H_

// Internal byte-stream plumbing shared by HCIVirtualTransport (the AT-facing
// side) and the btblecontroller port functions (ble/ble_hci_port.cpp).
// All functions are safe to call from both the AT task and the controller
// task; they take short critical sections internally.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Host -> controller stream (written by HCIVirtualTransport::write).
// Returns the number of bytes accepted; 0 means the queue is full and the
// caller should retry.
size_t hci_h2c_push(const uint8_t *buf, size_t size);

// Controller -> host stream (written by the BLE port, polled by
// HCIVirtualTransport::available()/read()).  If the queue cannot hold the
// whole push the packet is dropped whole to avoid corrupting the HCI framing;
// the return value mirrors the input size either way.
size_t hci_c2h_push(const uint8_t *buf, size_t size);

int  hci_c2h_available(void);
int  hci_c2h_read(void);
void hci_transport_reset(void);

// Port read arm/deliver (used by ble_hci_port.cpp).  Only one read may be
// armed at a time, matching the stock UART port behaviour.
void hci_port_arm_read(uint8_t *buf, uint32_t size,
                       void (*cb)(void *, uint8_t), void *dummy);
void hci_port_deliver(void);

// Diagnostics: [0]=armed flag, [1]=h2c bytes queued, [2]=c2h bytes queued,
// [3]=read arm calls, [4]=read callbacks fired, [5]=controller writes.
void hci_transport_state(uint32_t *out);
void hci_transport_note_write(void);

#ifdef __cplusplus
}
#endif

#endif
