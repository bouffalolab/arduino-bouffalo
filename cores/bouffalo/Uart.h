#pragma once

#include "api/Common.h"
#include "api/HardwareSerial.h"

struct bflb_device_s;

/*
 * BL616CL uart0/uart1 interface. Interrupts fill the RX ring; TX blocks on FIFO space. The variant
 * selects pins and instances.
 */
class Uart : public arduino::HardwareSerial {
public:
    /**
     * @brief Bind a UART object to its controller and variant TX/RX pins without starting the
     * peripheral.
     */
    Uart(uint8_t index, pin_size_t tx_pin, pin_size_t rx_pin);

    /**
     * @brief Configure the selected UART framing and RX ring, claiming the variant pins on
     * success.
     */
    void begin(unsigned long baudrate) override;
    /**
     * @brief Configure the selected UART framing and RX ring, claiming the variant pins on
     * success.
     */
    void begin(unsigned long baudrate, uint16_t config) override;
    /**
     * @brief Disable UART receive interrupts and release its RX ring.
     */
    void end() override;
    /**
     * @brief Return the currently readable byte count without consuming input.
     */
    int available(void) override;
    /**
     * @brief Return the next input byte without consuming it, or -1 when none is available.
     */
    int peek(void) override;
    /**
     * @brief Consume available input bytes; return the byte/count or the interface-specific
     * empty/error value.
     */
    int read(void) override;
    /**
     * @brief Wait until the UART transmitter has drained.
     */
    void flush(void) override;
    /**
     * @brief Return the number of bytes that can currently be queued without waiting.
     */
    int availableForWrite(void) override;
    /**
     * @brief Write the supplied byte or buffer and return the accepted byte count.
     */
    size_t write(uint8_t c) override;
    /**
     * @brief Write the supplied byte or buffer and return the accepted byte count.
     */
    size_t write(const uint8_t *buffer, size_t size) override;
    using Print::write;
    /**
     * @brief Report whether this object currently refers to a usable resource.
     */
    operator bool() override;

    /**
     * @brief BL extension: set RX capacity in bytes, default 256. Call before begin(); an
     * active UART returns false without changing its buffer.
     */
    bool setRxBufferSize(size_t size);

    /**
     * @brief BL extension: count RX bytes discarded when the ring is full; cleared by begin().
     */
    uint32_t rxOverflowCount(void) const;

private:
    /**
     * @brief Service UART receive interrupts and drain newly available FIFO bytes.
     */
    static void isr(int irq, void *arg);
    /**
     * @brief Move hardware RX bytes into the SPSC ring, counting overflow instead of
     * overwriting unread data.
     */
    void drain_rx_fifo(void);

    uint8_t index_;
    pin_size_t tx_pin_;
    pin_size_t rx_pin_;
    struct bflb_device_s *dev_ = nullptr;
    uint8_t *rx_buf_ = nullptr;
    size_t rx_size_ = 256;
    volatile size_t rx_head_ = 0; /* Written by the interrupt handler. */
    volatile size_t rx_tail_ = 0; /* Written by the reader. */
    volatile uint32_t rx_overflow_ = 0;
};

/*
 * The variant selects available serial instances. Whether Serial uses UART or USB is board-
 * specific; see USBSerial.h.
 */
#if defined(PIN_SERIAL_TX)
extern Uart Serial;
#endif
#if defined(PIN_SERIAL1_TX)
extern Uart Serial1;
#endif
#if defined(PIN_SERIAL2_TX)
extern Uart Serial2;
#endif
