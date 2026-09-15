#include "Arduino.h"

#include "bflb_irq.h"
#include "bflb_gpio.h"
#include "bflb_uart.h"
#include "pins_arduino.h"

extern "C" int pm_disable_gpio_keep(uint32_t pin);

HardwareSerial Serial(0, PIN_SERIAL_RX, PIN_SERIAL_TX);
HardwareSerial Serial1(1, PIN_SERIAL1_RX, PIN_SERIAL1_TX);

void serialEvent(void) __attribute__((weak));
void serialEvent1(void) __attribute__((weak));

static uint8_t data_bits(uint8_t config)
{
    return config & 0x03U;
}

static uint8_t stop_bits(uint8_t config)
{
    return (config >> 4U) & 0x03U;
}

static uint8_t parity(uint8_t config)
{
    return (config >> 6U) & 0x03U;
}

HardwareSerial::HardwareSerial(uint8_t index, int8_t rx_pin, int8_t tx_pin)
    : index_(index),
      rx_pin_(rx_pin),
      tx_pin_(tx_pin),
      baud_rate_(0U),
      device_(nullptr),
      rx_buffer_{},
      rx_head_(0U),
      rx_tail_(0U),
      rx_overflow_count_(0U)
{
}

void HardwareSerial::uartInterrupt(int, void *arg)
{
    HardwareSerial *serial = static_cast<HardwareSerial *>(arg);
    if (serial != nullptr) {
        serial->drainHardwareRx();
    }
}

void HardwareSerial::drainHardwareRx()
{
    if (device_ == nullptr) {
        return;
    }

    /*
     * bflb_uart_rxint_mask(..., false) enables RX FIFO threshold and RX
     * timeout interrupts. The timeout state remains set until acknowledged.
     */
    if ((bflb_uart_get_intstatus(device_) & UART_INTSTS_RTO) != 0U) {
        bflb_uart_int_clear(device_, UART_INTCLR_RTO);
    }

    while (bflb_uart_rxavailable(device_)) {
        const int value = bflb_uart_getchar(device_);
        if (value < 0) {
            break;
        }

        const uint16_t next = static_cast<uint16_t>(
            (rx_head_ + 1U) & kRxBufferMask);
        if (next == rx_tail_) {
            ++rx_overflow_count_;
            continue;
        }
        rx_buffer_[rx_head_] = static_cast<uint8_t>(value);
        rx_head_ = next;
    }
}

void HardwareSerial::resetRxBuffer()
{
    rx_head_ = 0U;
    rx_tail_ = 0U;
    rx_overflow_count_ = 0U;
}

uint16_t HardwareSerial::bufferedRxCount() const
{
    return static_cast<uint16_t>((rx_head_ - rx_tail_) & kRxBufferMask);
}

void HardwareSerial::begin(unsigned long baud, uint8_t config)
{
    struct bflb_device_s *gpio = bflb_device_get_by_name("gpio");
    device_ = bflb_device_get_by_name(
        index_ == 0 ? "uart0" : (index_ == 1 ? "uart1" : "uart2"));
    if ((gpio == nullptr) || (device_ == nullptr)) {
        return;
    }

    /*
     * UART pins must leave the BL616CL low-power retention domain before
     * their mux is assigned. The SDK console follows the same sequence.
     */
    pm_disable_gpio_keep(tx_pin_);
    pm_disable_gpio_keep(rx_pin_);

    switch (index_) {
        case 0:
            bflb_gpio_uart_init(gpio, tx_pin_, GPIO_UART_FUNC_UART0_TX);
            bflb_gpio_uart_init(gpio, rx_pin_, GPIO_UART_FUNC_UART0_RX);
            break;
        case 1:
            bflb_gpio_uart_init(gpio, tx_pin_, GPIO_UART_FUNC_UART1_TX);
            bflb_gpio_uart_init(gpio, rx_pin_, GPIO_UART_FUNC_UART1_RX);
            break;
        case 2:
        default:
            bflb_gpio_uart_init(gpio, tx_pin_, GPIO_UART_FUNC_UART2_TX);
            bflb_gpio_uart_init(gpio, rx_pin_, GPIO_UART_FUNC_UART2_RX);
            break;
    }

    struct bflb_uart_config_s uart_config = {};
    baud_rate_ = static_cast<uint32_t>(baud);
    uart_config.baudrate = baud;
    uart_config.direction = UART_DIRECTION_TXRX;
    uart_config.data_bits = data_bits(config);
    uart_config.stop_bits = stop_bits(config);
    uart_config.parity = parity(config);
    uart_config.bit_order = UART_LSB_FIRST;
    uart_config.flow_ctrl = UART_FLOWCTRL_NONE;
    uart_config.tx_fifo_threshold = 7;
    uart_config.rx_fifo_threshold = 1;
    bflb_uart_init(device_, &uart_config);
    resetRxBuffer();
    bflb_uart_rxint_mask(device_, false);
    (void)bflb_irq_attach(device_->irq_num, uartInterrupt, this);
    bflb_irq_enable(device_->irq_num);
}

void HardwareSerial::begin(unsigned long baud, uint8_t config,
                           int8_t rxPin, int8_t txPin)
{
#ifdef BL616CL_STAGE1
    // UNO R4 bridge uses pin-coded ESP32 calls for its two hardware UARTs:
    //   Serial.begin(..., 44, 43) -> RA4M1 bridge UART1 (GPIO24/25)
    //   Serial1.begin(..., 6, 5)  -> AT command UART2 (GPIO10/11)
    // This keeps SDK UART0 (GPIO34/35) untouched for the debug console.
    if (rxPin == 44 && txPin == 43) {
        index_ = 1;
        rx_pin_ = PIN_SERIAL1_RX;
        tx_pin_ = PIN_SERIAL1_TX;
    } else if (rxPin == 6 && txPin == 5) {
        // TODO(bl616cl): route AT to UART2 after enabling its peripheral
        // clock. Until then keep Serial1 on UART1 so the debug console on
        // UART0 remains untouched.
    }
#else
    (void)rxPin;
    (void)txPin;
#endif
    begin(baud, config);
}

bool HardwareSerial::updateBaudRate(unsigned long baud)
{
    if (device_ == nullptr || baud == 0UL) {
        return false;
    }

    flush();
    if (bflb_uart_feature_control(
            device_,
            UART_CMD_SET_BAUD_RATE,
            static_cast<size_t>(baud)) != 0) {
        return false;
    }
    baud_rate_ = static_cast<uint32_t>(baud);
    clearRx();
    return true;
}

void HardwareSerial::clearRx()
{
    if (device_ != nullptr) {
        const uintptr_t irq_state = bflb_irq_save();
        (void)bflb_uart_feature_control(device_, UART_CMD_CLR_RX_FIFO, 0);
        resetRxBuffer();
        bflb_irq_restore(irq_state);
    } else {
        resetRxBuffer();
    }
}

void HardwareSerial::end()
{
    if (device_ != nullptr) {
        bflb_irq_disable(device_->irq_num);
        (void)bflb_irq_detach(device_->irq_num);
        flush();
        bflb_uart_deinit(device_);
        device_ = nullptr;
    }
    resetRxBuffer();
}

int HardwareSerial::available()
{
    if (device_ == nullptr) {
        return 0;
    }
    return static_cast<int>(bufferedRxCount());
}

int HardwareSerial::peek()
{
    if (device_ == nullptr || rx_head_ == rx_tail_) {
        return -1;
    }
    return rx_buffer_[rx_tail_];
}

int HardwareSerial::read()
{
    if (device_ == nullptr || rx_head_ == rx_tail_) {
        return -1;
    }
    const int value = rx_buffer_[rx_tail_];
    rx_tail_ = static_cast<uint16_t>((rx_tail_ + 1U) & kRxBufferMask);
    return value;
}

size_t HardwareSerial::read(uint8_t *buffer, size_t size)
{
    if (device_ == nullptr || buffer == nullptr || size == 0) {
        return 0;
    }

    size_t count = 0;
    while (count < size) {
        int value = read();
        if (value < 0) {
            break;
        }
        buffer[count++] = static_cast<uint8_t>(value);
    }
    return count;
}

int HardwareSerial::availableForWrite()
{
    if (device_ == nullptr) {
        return 0;
    }
    int free_slots = bflb_uart_feature_control(device_, UART_CMD_GET_TX_FIFO_CNT, 0);
    if (free_slots < 0) {
        return 0;
    }
    return free_slots > UART_FIFO_MAX ? UART_FIFO_MAX : free_slots;
}

void HardwareSerial::flush()
{
    if (device_ != nullptr) {
        (void)bflb_uart_wait_tx_done(device_);
    }
}

size_t HardwareSerial::write(uint8_t value)
{
    if (device_ == nullptr || bflb_uart_putchar(device_, value) < 0) {
        setWriteError();
        return 0;
    }
    return 1;
}

size_t HardwareSerial::write(const uint8_t *buffer, size_t size)
{
    if (device_ == nullptr || buffer == nullptr) {
        setWriteError();
        return 0;
    }

    size_t written = 0;
    while (written < size) {
        if (bflb_uart_putchar(device_, buffer[written]) < 0) {
            setWriteError();
            break;
        }
        ++written;
    }
    return written;
}

void serialEventRun(void)
{
    if (serialEvent && Serial.available()) {
        serialEvent();
    }
    if (serialEvent1 && Serial1.available()) {
        serialEvent1();
    }
}
