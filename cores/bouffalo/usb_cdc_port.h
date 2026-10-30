#pragma once

/*
 * USB CDC ACM backend for Serial. Enable when boards.txt supplies USB_VID, USB_PID,
 * USB_MANUFACTURER and USB_PRODUCT. usb_cdc_port.c owns descriptors, buffers, touch reset and line
 * callbacks.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Host line settings, using CDC SET_LINE_CODING values. */
struct usb_cdc_port_line {
    uint32_t baud;
    uint8_t stop_bits; /* 0: one stop bit; 1: one and a half; 2: two. */
    uint8_t parity;    /* 0: none; 1: odd; 2: even; 3: mark; 4: space. */
    uint8_t data_bits;
    bool dtr;
    bool rts;
};

/**
 * @brief Register descriptors and start USB once after the scheduler starts.
 */
void usb_cdc_port_init(void);

/**
 * @brief True when configured and the host has asserted DTR to open the port.
 */
bool usb_cdc_port_connected(void);
/**
 * @brief Copy the host CDC settings and control signals into the caller's structure.
 */
void usb_cdc_port_get_line(struct usb_cdc_port_line *line);

/**
 * @brief Enable 1200 bps touch by default; disabling cancels a scheduled reset that has not
 * executed.
 */
void usb_cdc_port_set_touch_reset(bool enable);
/**
 * @brief Deliver each line/control request once in the line task; NULL unregisters. Do not
 * replay requests predating registration. Create the task on first registration and return
 * false if creation fails.
 *
 * @param callback Task-context callback, or NULL to unregister. The callback may block.
 * @return true on registration/unregistration; false if the line task cannot be created.
 */
bool usb_cdc_port_on_line_change(void (*callback)(void));

/**
 * @brief Return the number of unread bytes in the CDC RX ring.
 */
size_t usb_cdc_port_rx_count(void);
/**
 * @brief Return the next CDC input byte without consuming it, or -1 if empty.
 */
int usb_cdc_port_peek(void);
/**
 * @brief Consume the next CDC input byte, or return -1 if empty.
 */
int usb_cdc_port_read(void);

/**
 * @brief Queue output bytes. Return zero and discard if closed. Wait for space when full;
 * return the accepted byte count after 100 ms without progress.
 */
size_t usb_cdc_port_write(const uint8_t *buf, size_t len);
/**
 * @brief Return currently available CDC transmit-ring capacity.
 */
size_t usb_cdc_port_tx_space(void);
/**
 * @brief Drain queued output; return if the port closes or no progress occurs for 100 ms.
 */
void usb_cdc_port_flush(void);

#ifdef __cplusplus
}
#endif
