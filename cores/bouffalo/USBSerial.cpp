/* Serial（USB CDC）的 Arduino 接口；USB 设备、缓冲与 1200 bps touch 在 usb_cdc_port.c */
#include "Arduino.h"

#if defined(USB_VID)

#include "usb_cdc_port.h"

USBSerial Serial;

void USBSerial::begin(unsigned long baudrate)
{
    (void)baudrate;
}

void USBSerial::begin(unsigned long baudrate, uint16_t config)
{
    (void)baudrate;
    (void)config;
}

void USBSerial::end()
{
}

int USBSerial::available(void)
{
    return (int)usb_cdc_port_rx_count();
}

int USBSerial::peek(void)
{
    return usb_cdc_port_peek();
}

int USBSerial::read(void)
{
    return usb_cdc_port_read();
}

void USBSerial::flush(void)
{
    usb_cdc_port_flush();
}

int USBSerial::availableForWrite(void)
{
    return (int)usb_cdc_port_tx_space();
}

size_t USBSerial::write(uint8_t c)
{
    return write(&c, 1);
}

size_t USBSerial::write(const uint8_t *buffer, size_t size)
{
    size_t n = usb_cdc_port_write(buffer, size);
    if (n < size) {
        setWriteError();
    }
    return n;
}

/*
 * 与官方 AVR、SAMD 核心一样延时 10 ms 再返回：主机刚置 DTR 时还在配置端口（Linux 随后切换 raw 模式并清空输入），
 * 这时立即输出会丢失。
 */
USBSerial::operator bool()
{
    bool connected = usb_cdc_port_connected();
    delay(10);
    return connected;
}

static struct usb_cdc_port_line line_state(void)
{
    struct usb_cdc_port_line line;
    usb_cdc_port_get_line(&line);
    return line;
}

uint32_t USBSerial::baud(void)
{
    return line_state().baud;
}

uint8_t USBSerial::stopbits(void)
{
    return line_state().stop_bits;
}

uint8_t USBSerial::paritytype(void)
{
    return line_state().parity;
}

uint8_t USBSerial::numbits(void)
{
    return line_state().data_bits;
}

bool USBSerial::dtr(void)
{
    return line_state().dtr;
}

bool USBSerial::rts(void)
{
    return line_state().rts;
}

#endif /* USB_VID */
