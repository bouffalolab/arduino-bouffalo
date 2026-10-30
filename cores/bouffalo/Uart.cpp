/*
 * Arduino UART implementation. RX threshold and receive-timeout interrupts move FIFO bytes into a
 * single-producer/single-consumer ring: only the ISR writes, only the reader consumes, so no lock
 * is needed. TX writes one byte at a time, waiting for FIFO space; lhal waits up to 100 ms per
 * byte.
 */
#include <stdlib.h>

#include "bflb_clock.h"
#include "bflb_gpio.h"
#include "bflb_irq.h"
#include "bflb_uart.h"
#include "pin_keep.h"
#include "pin_map.h"
#include "uart_reset.h"

#include "Arduino.h"

/* Receive timeout in bit periods: drain a partial FIFO after roughly four idle character times. */
#define UART_RX_TIMEOUT_BITS 40

/* Package board_init enables clocks only for uart0 and uart1. */
static const char *const uart_names[] = { "uart0", "uart1" };
static const uint8_t uart_tx_funcs[] = { GPIO_UART_FUNC_UART0_TX, GPIO_UART_FUNC_UART1_TX };
static const uint8_t uart_rx_funcs[] = { GPIO_UART_FUNC_UART0_RX, GPIO_UART_FUNC_UART1_RX };

/**
 * @brief Bind a UART object to its controller and variant TX/RX pins without starting the
 * peripheral.
 */
Uart::Uart(uint8_t index, pin_size_t tx_pin, pin_size_t rx_pin)
    : index_(index), tx_pin_(tx_pin), rx_pin_(rx_pin)
{
}

/**
 * @brief Configure the selected UART framing and RX ring, claiming the variant pins on
 * success.
 */
void Uart::begin(unsigned long baudrate)
{
    begin(baudrate, SERIAL_8N1);
}

/**
 * @brief Validate and translate Arduino UART framing to the supported lhal configuration.
 */
static bool to_lhal_config(uint16_t config, struct bflb_uart_config_s *cfg)
{
    switch (config & SERIAL_DATA_MASK) {
        case SERIAL_DATA_5: cfg->data_bits = UART_DATA_BITS_5; break;
        case SERIAL_DATA_6: cfg->data_bits = UART_DATA_BITS_6; break;
        case SERIAL_DATA_7: cfg->data_bits = UART_DATA_BITS_7; break;
        case SERIAL_DATA_8: cfg->data_bits = UART_DATA_BITS_8; break;
        default: return false;
    }
    switch (config & SERIAL_PARITY_MASK) {
        case SERIAL_PARITY_NONE: cfg->parity = UART_PARITY_NONE; break;
        case SERIAL_PARITY_ODD: cfg->parity = UART_PARITY_ODD; break;
        case SERIAL_PARITY_EVEN: cfg->parity = UART_PARITY_EVEN; break;
        default: return false; /*
 * bflb_uart_init does not support MARK or SPACE parity and would leave the previous parity
 * unchanged.
 */
    }
    switch (config & SERIAL_STOP_BIT_MASK) {
        case SERIAL_STOP_BIT_1: cfg->stop_bits = UART_STOP_BITS_1; break;
        case SERIAL_STOP_BIT_1_5: cfg->stop_bits = UART_STOP_BITS_1_5; break;
        case SERIAL_STOP_BIT_2: cfg->stop_bits = UART_STOP_BITS_2; break;
        default: return false;
    }
    return true;
}

/**
 * @brief Reject invalid UART indices, reserved pins, frame formats or unrepresentable baud
 * divisors. The UART remains closed and operator bool returns false.
 */
void Uart::begin(unsigned long baudrate, uint16_t config)
{
    struct bflb_uart_config_s cfg = {};
    int tx_gpio = pin_to_gpio(tx_pin_);
    int rx_gpio = pin_to_gpio(rx_pin_);

    end();
    if (index_ >= sizeof(uart_names) / sizeof(uart_names[0]) || baudrate == 0 ||
        tx_gpio < 0 || rx_gpio < 0 || !to_lhal_config(config, &cfg)) {
        return;
    }
    struct bflb_device_s *dev = bflb_device_get_by_name(uart_names[index_]);
    struct bflb_device_s *gpio = bflb_device_get_by_name("gpio");
    if (dev == nullptr || gpio == nullptr) {
        return;
    }
    /*
 * bflb_uart_init truncates clock/baud to a divisor in 1..0xfffe; it does not report an out-of-
 * range value.
 */
    uint32_t clk = bflb_clk_get_peripheral_clock(BFLB_DEVICE_TYPE_UART, dev->idx);
    uint64_t div = ((uint64_t)clk * 10 / baudrate + 5) / 10;
    if (div < 1 || div >= 0xffff) {
        return;
    }
    if (rx_buf_ == nullptr) {
        rx_buf_ = static_cast<uint8_t *>(malloc(rx_size_));
        if (rx_buf_ == nullptr) {
            return;
        }
    }
    rx_head_ = rx_tail_ = 0;
    rx_overflow_ = 0;

    /*
 * Warm reset, including the uploader reset, does not reset peripherals. Incoming data during ROM
 * download can leave URX_BUS_BUSY set; bflb_uart_init does not clear it and subsequent RX stops.
 * Reset the controller first.
 */
    uart_reset(index_);
    pin_release_keep(tx_gpio);
    pin_release_keep(rx_gpio);
    bflb_gpio_uart_init(gpio, tx_gpio, uart_tx_funcs[index_]);
    bflb_gpio_uart_init(gpio, rx_gpio, uart_rx_funcs[index_]);

    cfg.baudrate = baudrate;
    cfg.flow_ctrl = 0;
    cfg.tx_fifo_threshold = 7;
    cfg.rx_fifo_threshold = 7;
    cfg.bit_order = UART_LSB_FIRST;
    bflb_uart_init(dev, &cfg);
    bflb_uart_feature_control(dev, UART_CMD_SET_RTO_VALUE, UART_RX_TIMEOUT_BITS);

    dev_ = dev;
    bflb_irq_attach(dev->irq_num, isr, this);
    bflb_uart_rxint_mask(dev, false);
    bflb_irq_enable(dev->irq_num);
}

/**
 * @brief Disable UART receive interrupts and release its RX ring.
 */
void Uart::end()
{
    if (dev_ == nullptr) {
        return;
    }
    flush();
    bflb_irq_disable(dev_->irq_num);
    bflb_uart_rxint_mask(dev_, true);
    bflb_irq_detach(dev_->irq_num);
    bflb_uart_deinit(dev_);
    dev_ = nullptr;
}

/**
 * @brief Change RX ring capacity before begin; reject changes while the UART is active.
 */
bool Uart::setRxBufferSize(size_t size)
{
    if (dev_ != nullptr || size < 2) {
        return false;
    }
    free(rx_buf_);
    rx_buf_ = nullptr;
    rx_size_ = size;
    return true;
}

/**
 * @brief Return the number of RX bytes dropped since the latest begin.
 */
uint32_t Uart::rxOverflowCount(void) const
{
    return rx_overflow_;
}

/**
 * @brief Reserve one slot to distinguish full from empty; usable capacity is rx_size_ - 1
 * bytes.
 */
void Uart::drain_rx_fifo(void)
{
    while (bflb_uart_rxavailable(dev_)) {
        uint8_t c = (uint8_t)bflb_uart_getchar(dev_);
        size_t next = (rx_head_ + 1) % rx_size_;
        if (next == rx_tail_) {
            rx_overflow_ = rx_overflow_ + 1;
            continue;
        }
        rx_buf_[rx_head_] = c;
        __atomic_signal_fence(__ATOMIC_RELEASE); /* Store data before publishing head. */
        rx_head_ = next;
    }
}

/**
 * @brief Service UART receive interrupts and drain newly available FIFO bytes.
 */
void Uart::isr(int irq, void *arg)
{
    (void)irq;
    Uart *self = static_cast<Uart *>(arg);
    uint32_t status = bflb_uart_get_intstatus(self->dev_);

    /*
 * Clear RTO before draining the FIFO, as in SDK examples, so bytes arriving afterward can trigger
 * a new timeout.
 */
    if (status & UART_INTSTS_RTO) {
        bflb_uart_int_clear(self->dev_, UART_INTCLR_RTO);
    }
    if (status & (UART_INTSTS_RX_FIFO | UART_INTSTS_RTO)) {
        self->drain_rx_fifo();
    }
}

/**
 * @brief Return the currently readable byte count without consuming input.
 */
int Uart::available(void)
{
    if (dev_ == nullptr) {
        return 0;
    }
    return (int)((rx_head_ + rx_size_ - rx_tail_) % rx_size_);
}

/**
 * @brief Return the next input byte without consuming it, or -1 when none is available.
 */
int Uart::peek(void)
{
    if (available() == 0) {
        return -1;
    }
    __atomic_signal_fence(__ATOMIC_ACQUIRE); /* Read head before reading the corresponding data. */
    return rx_buf_[rx_tail_];
}

/**
 * @brief Consume available input bytes; return the byte/count or the interface-specific
 * empty/error value.
 */
int Uart::read(void)
{
    if (available() == 0) {
        return -1;
    }
    __atomic_signal_fence(__ATOMIC_ACQUIRE);
    uint8_t c = rx_buf_[rx_tail_];
    __atomic_signal_fence(__ATOMIC_RELEASE); /* Release the slot only after consuming its data. */
    rx_tail_ = (rx_tail_ + 1) % rx_size_;
    return c;
}

/**
 * @brief Wait until the UART transmitter has drained.
 */
void Uart::flush(void)
{
    if (dev_ != nullptr) {
        bflb_uart_wait_tx_done(dev_);
    }
}

/**
 * @brief Number of free TX FIFO slots.
 */
int Uart::availableForWrite(void)
{
    if (dev_ == nullptr) {
        return 0;
    }
    return bflb_uart_feature_control(dev_, UART_CMD_GET_TX_FIFO_CNT, 0);
}

/**
 * @brief Write the supplied byte or buffer and return the accepted byte count.
 */
size_t Uart::write(uint8_t c)
{
    return write(&c, 1);
}

/**
 * @brief Write the supplied byte or buffer and return the accepted byte count.
 */
size_t Uart::write(const uint8_t *buffer, size_t size)
{
    if (dev_ == nullptr) {
        return 0;
    }
    for (size_t i = 0; i < size; i++) {
        if (bflb_uart_putchar(dev_, buffer[i]) < 0) {
            setWriteError();
            return i;
        }
    }
    return size;
}

/**
 * @brief Report whether this object currently refers to a usable resource.
 */
Uart::operator bool()
{
    return dev_ != nullptr;
}
