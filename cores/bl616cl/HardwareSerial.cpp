#include "Arduino.h"

#include "bflb_gpio.h"
#include "bflb_uart.h"
#include "pins_arduino.h"

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
    : index_(index), rx_pin_(rx_pin), tx_pin_(tx_pin), peeked_(-1), device_(nullptr)
{
}

void HardwareSerial::begin(unsigned long baud, uint8_t config)
{
    struct bflb_device_s *gpio = bflb_device_get_by_name("gpio");
    device_ = bflb_device_get_by_name(index_ == 0 ? "uart0" : "uart1");
    if ((gpio == nullptr) || (device_ == nullptr)) {
        return;
    }

    if (index_ == 0) {
        bflb_gpio_uart_init(gpio, tx_pin_, GPIO_UART_FUNC_UART0_TX);
        bflb_gpio_uart_init(gpio, rx_pin_, GPIO_UART_FUNC_UART0_RX);
    } else {
        bflb_gpio_uart_init(gpio, tx_pin_, GPIO_UART_FUNC_UART1_TX);
        bflb_gpio_uart_init(gpio, rx_pin_, GPIO_UART_FUNC_UART1_RX);
    }

    struct bflb_uart_config_s uart_config = {};
    uart_config.baudrate = baud;
    uart_config.direction = UART_DIRECTION_TXRX;
    uart_config.data_bits = data_bits(config);
    uart_config.stop_bits = stop_bits(config);
    uart_config.parity = parity(config);
    uart_config.bit_order = UART_LSB_FIRST;
    uart_config.flow_ctrl = UART_FLOWCTRL_NONE;
    uart_config.tx_fifo_threshold = 7;
    uart_config.rx_fifo_threshold = 7;
    bflb_uart_init(device_, &uart_config);
    peeked_ = -1;
}

void HardwareSerial::end()
{
    if (device_ != nullptr) {
        flush();
        bflb_uart_deinit(device_);
        device_ = nullptr;
    }
    peeked_ = -1;
}

int HardwareSerial::available()
{
    if (device_ == nullptr) {
        return 0;
    }
    int count = bflb_uart_feature_control(device_, UART_CMD_GET_RX_FIFO_CNT, 0);
    return count + (peeked_ >= 0 ? 1 : 0);
}

int HardwareSerial::peek()
{
    if (peeked_ < 0 && device_ != nullptr) {
        peeked_ = bflb_uart_getchar(device_);
    }
    return peeked_;
}

int HardwareSerial::read()
{
    if (peeked_ >= 0) {
        int value = peeked_;
        peeked_ = -1;
        return value;
    }
    return device_ == nullptr ? -1 : bflb_uart_getchar(device_);
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
