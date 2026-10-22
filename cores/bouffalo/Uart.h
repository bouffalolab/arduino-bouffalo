#pragma once

#include "api/Common.h"
#include "api/HardwareSerial.h"

struct bflb_device_s;

/*
 * BL616CL 的 UART（uart0、uart1）。接收由中断写入环形缓冲，发送阻塞写 FIFO。
 * 引脚与实例由 variant 决定。
 */
class Uart : public arduino::HardwareSerial {
public:
    Uart(uint8_t index, pin_size_t tx_pin, pin_size_t rx_pin);

    void begin(unsigned long baudrate) override;
    void begin(unsigned long baudrate, uint16_t config) override;
    void end() override;
    int available(void) override;
    int peek(void) override;
    int read(void) override;
    void flush(void) override;
    int availableForWrite(void) override;
    size_t write(uint8_t c) override;
    size_t write(const uint8_t *buffer, size_t size) override;
    using Print::write;
    operator bool() override;

    /*
     * BL 扩展：接收缓冲大小（字节），默认 256。只能在 begin() 之前调用；
     * 已经 begin() 时返回 false，缓冲不变。
     */
    bool setRxBufferSize(size_t size);

    /* BL 扩展：缓冲满后丢弃的接收字节数，begin() 时清零。 */
    uint32_t rxOverflowCount(void) const;

private:
    static void isr(int irq, void *arg);
    void drain_rx_fifo(void);

    uint8_t index_;
    pin_size_t tx_pin_;
    pin_size_t rx_pin_;
    struct bflb_device_s *dev_ = nullptr;
    uint8_t *rx_buf_ = nullptr;
    size_t rx_size_ = 256;
    volatile size_t rx_head_ = 0; /* 中断写 */
    volatile size_t rx_tail_ = 0; /* 读者写 */
    volatile uint32_t rx_overflow_ = 0;
};

#if defined(PIN_SERIAL_TX)
extern Uart Serial;
#endif
