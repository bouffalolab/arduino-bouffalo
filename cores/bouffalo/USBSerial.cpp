/*
 * Arduino interface for USB CDC Serial. usb_cdc_port.c owns the device, buffers, 1200 bps touch
 * and line-change callbacks.
 */
#include "Arduino.h"

#if defined(USB_VID)

#include "usb_cdc_port.h"

USBSerial Serial;

/**
 * @brief Accept Arduino serial initialization arguments; USB enumeration and host line
 * settings remain owned by the CDC port.
 */
void USBSerial::begin(unsigned long baudrate)
{
    (void)baudrate;
}

/**
 * @brief Accept Arduino serial initialization arguments; USB enumeration and host line
 * settings remain owned by the CDC port.
 */
void USBSerial::begin(unsigned long baudrate, uint16_t config)
{
    (void)baudrate;
    (void)config;
}

/**
 * @brief Leave the Core-owned USB device active; this compatibility call does not tear down
 * CDC.
 */
void USBSerial::end()
{
}

/**
 * @brief Return the currently readable byte count without consuming input.
 */
int USBSerial::available(void)
{
    return (int)usb_cdc_port_rx_count();
}

/**
 * @brief Return the next input byte without consuming it, or -1 when none is available.
 */
int USBSerial::peek(void)
{
    return usb_cdc_port_peek();
}

/**
 * @brief Consume available input bytes; return the byte/count or the interface-specific
 * empty/error value.
 */
int USBSerial::read(void)
{
    return usb_cdc_port_read();
}

/**
 * @brief Wait for pending CDC output with the port's bounded disconnect/stall policy.
 */
void USBSerial::flush(void)
{
    usb_cdc_port_flush();
}

/**
 * @brief Return the number of bytes that can currently be queued without waiting.
 */
int USBSerial::availableForWrite(void)
{
    return (int)usb_cdc_port_tx_space();
}

/**
 * @brief Write the supplied byte or buffer and return the accepted byte count.
 */
size_t USBSerial::write(uint8_t c)
{
    return write(&c, 1);
}

/**
 * @brief Write the supplied byte or buffer and return the accepted byte count.
 */
size_t USBSerial::write(const uint8_t *buffer, size_t size)
{
    size_t n = usb_cdc_port_write(buffer, size);
    if (n < size) {
        setWriteError();
    }
    return n;
}

/**
 * @brief Delay 10 ms, as in the official AVR and SAMD cores. After asserting DTR, Linux still
 * configures raw mode and clears input; immediate output would be lost.
 */
USBSerial::operator bool()
{
    bool connected = usb_cdc_port_connected();
    delay(10);
    return connected;
}

/**
 * @brief Snapshot the current USB CDC host line settings.
 */
static struct usb_cdc_port_line line_state(void)
{
    struct usb_cdc_port_line line;
    usb_cdc_port_get_line(&line);
    return line;
}

/**
 * @brief Return the host-selected USB baud rate.
 */
uint32_t USBSerial::baud(void)
{
    return line_state().baud;
}

/**
 * @brief Return the CDC stop-bit encoding selected by the host.
 */
uint8_t USBSerial::stopbits(void)
{
    return line_state().stop_bits;
}

/**
 * @brief Return the CDC parity encoding selected by the host.
 */
uint8_t USBSerial::paritytype(void)
{
    return line_state().parity;
}

/**
 * @brief Return the host-selected number of data bits.
 */
uint8_t USBSerial::numbits(void)
{
    return line_state().data_bits;
}

/**
 * @brief Return the current host DTR state.
 */
bool USBSerial::dtr(void)
{
    return line_state().dtr;
}

/**
 * @brief Return the current host RTS state.
 */
bool USBSerial::rts(void)
{
    return line_state().rts;
}

/**
 * @brief Enable or cancel Core handling of the 1200 bps download request.
 */
void USBSerial::setTouchReset(bool enable)
{
    usb_cdc_port_set_touch_reset(enable);
}

/**
 * @brief Register a task-context line-change callback; return false if task creation fails.
 */
bool USBSerial::onLineChange(void (*callback)(void))
{
    return usb_cdc_port_on_line_change(callback);
}

#endif /* USB_VID */
