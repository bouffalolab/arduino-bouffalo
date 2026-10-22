/*
 * UART 的 Arduino 实现。接收：RX FIFO 达到阈值或接收超时（RTO）时进中断，
 * 把 FIFO 中的字节搬进环形缓冲；缓冲只有中断写、读者读，单生产者单消费者，无需加锁。
 * 发送：逐字节写 FIFO，FIFO 满时等待（lhal 每字节最多等 100 ms）。
 */
#include <stdlib.h>

#include "bflb_clock.h"
#include "bflb_gpio.h"
#include "bflb_irq.h"
#include "bflb_uart.h"
#include "pin_keep.h"
#include "pin_map.h"

#include "Arduino.h"

/* 接收超时，单位为位时间：线路空闲约 4 个字符后把不足阈值的字节取走 */
#define UART_RX_TIMEOUT_BITS 40

/* 底层包的 board_init 只打开 uart0、uart1 的时钟 */
static const char *const uart_names[] = { "uart0", "uart1" };
static const uint8_t uart_tx_funcs[] = { GPIO_UART_FUNC_UART0_TX, GPIO_UART_FUNC_UART1_TX };
static const uint8_t uart_rx_funcs[] = { GPIO_UART_FUNC_UART0_RX, GPIO_UART_FUNC_UART1_RX };

Uart::Uart(uint8_t index, pin_size_t tx_pin, pin_size_t rx_pin)
    : index_(index), tx_pin_(tx_pin), rx_pin_(rx_pin)
{
}

void Uart::begin(unsigned long baudrate)
{
    begin(baudrate, SERIAL_8N1);
}

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
        default: return false; /* MARK、SPACE：bflb_uart_init 不支持，会保留原来的校验设置 */
    }
    switch (config & SERIAL_STOP_BIT_MASK) {
        case SERIAL_STOP_BIT_1: cfg->stop_bits = UART_STOP_BITS_1; break;
        case SERIAL_STOP_BIT_1_5: cfg->stop_bits = UART_STOP_BITS_1_5; break;
        case SERIAL_STOP_BIT_2: cfg->stop_bits = UART_STOP_BITS_2; break;
        default: return false;
    }
    return true;
}

/* 参数无效（UART 序号、不可用的引脚、帧格式、分频器放不下的波特率）时不打开串口，operator bool 为 false。 */
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
    /* bflb_uart_init 按 时钟/波特率 取整得到分频值，有效范围 1 到 0xfffe，越界时不报错 */
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

uint32_t Uart::rxOverflowCount(void) const
{
    return rx_overflow_;
}

/* 环形缓冲留一个空位区分满和空，可存 rx_size_ - 1 字节 */
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
        __atomic_signal_fence(__ATOMIC_RELEASE); /* 先写数据再发布 head */
        rx_head_ = next;
    }
}

void Uart::isr(int irq, void *arg)
{
    (void)irq;
    Uart *self = static_cast<Uart *>(arg);
    uint32_t status = bflb_uart_get_intstatus(self->dev_);

    /* 与 SDK 示例相同，先清 RTO 再读 FIFO：读空之后到达的字节会重新触发 RTO */
    if (status & UART_INTSTS_RTO) {
        bflb_uart_int_clear(self->dev_, UART_INTCLR_RTO);
    }
    if (status & (UART_INTSTS_RX_FIFO | UART_INTSTS_RTO)) {
        self->drain_rx_fifo();
    }
}

int Uart::available(void)
{
    if (dev_ == nullptr) {
        return 0;
    }
    return (int)((rx_head_ + rx_size_ - rx_tail_) % rx_size_);
}

int Uart::peek(void)
{
    if (available() == 0) {
        return -1;
    }
    __atomic_signal_fence(__ATOMIC_ACQUIRE); /* 读到 head 之后再读数据 */
    return rx_buf_[rx_tail_];
}

int Uart::read(void)
{
    if (available() == 0) {
        return -1;
    }
    __atomic_signal_fence(__ATOMIC_ACQUIRE);
    uint8_t c = rx_buf_[rx_tail_];
    __atomic_signal_fence(__ATOMIC_RELEASE); /* 读完数据再让出这个位置 */
    rx_tail_ = (rx_tail_ + 1) % rx_size_;
    return c;
}

void Uart::flush(void)
{
    if (dev_ != nullptr) {
        bflb_uart_wait_tx_done(dev_);
    }
}

/* TX FIFO 的空位数 */
int Uart::availableForWrite(void)
{
    if (dev_ == nullptr) {
        return 0;
    }
    return bflb_uart_feature_control(dev_, UART_CMD_GET_TX_FIFO_CNT, 0);
}

size_t Uart::write(uint8_t c)
{
    return write(&c, 1);
}

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

Uart::operator bool()
{
    return dev_ != nullptr;
}
