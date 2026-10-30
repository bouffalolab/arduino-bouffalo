#include "uart_transport.h"

#include "bflb_irq.h"
#include "bflb_core.h"
#include "bflb_uart.h"
#include "hardware/uart_reg.h"
#if defined(BL616CL_AT_RX_DMA) && BL616CL_AT_RX_DMA
#include "bflb_dma.h"
#include "dma_rx_cursor.h"
#endif

namespace bridge {
#if defined(BL616CL_AT_RX_DMA) && BL616CL_AT_RX_DMA
namespace {
constexpr uint32_t kDmaRxBytes = UartTransport::kRxCapacity;
ATTR_NOCACHE_NOINIT_RAM_SECTION uint8_t dma_rx_bytes[kDmaRxBytes];
ATTR_NOCACHE_NOINIT_RAM_SECTION bflb_dma_channel_lli_pool_s dma_rx_lli[2];
UartTransport *dma_rx_owner = nullptr;
}
#endif

static_assert(UartTransport::kRxCapacity == kAtUartRxBufferBytes, "RX buffer size must match bridge configuration");
static_assert(UartTransport::kTxCapacity == kAtUartTxBufferBytes, "TX buffer size must match bridge configuration");

bool UartTransport::begin(HardwareSerial &serial)
{
    if (device_ != nullptr) {
        return true;
    }

    device_ = serial.nativeHandle();
    if (device_ == nullptr) {
        return false;
    }

    if (bflb_irq_attach(device_->irq_num, &UartTransport::irq_handler, this) != 0) {
        device_ = nullptr;
        return false;
    }

    (void)bflb_uart_feature_control(device_, UART_CMD_SET_RTO_VALUE, 0x80U);
#if defined(BL616CL_AT_RX_DMA) && BL616CL_AT_RX_DMA
    if (!begin_rx_dma()) {
        (void)bflb_irq_detach(device_->irq_num);
        device_ = nullptr;
        return false;
    }
#endif
    bflb_uart_rxint_mask(device_, false);
    bflb_irq_enable(device_->irq_num);
#if !defined(BL616CL_AT_RX_DMA) || !BL616CL_AT_RX_DMA
    drain_rx_fifo();
#endif
    return true;
}

void UartTransport::end()
{
    if (device_ == nullptr) {
        return;
    }

    bflb_uart_rxint_mask(device_, true);
    bflb_irq_disable(device_->irq_num);
#if defined(BL616CL_AT_RX_DMA) && BL616CL_AT_RX_DMA
    end_rx_dma();
#endif
    (void)bflb_irq_detach(device_->irq_num);
    device_ = nullptr;
#if !defined(BL616CL_AT_RX_DMA) || !BL616CL_AT_RX_DMA
    rx_.clear();
#else
    dma_available_ = 0U;
    dma_read_offset_ = dma_last_offset_;
#endif
    tx_.clear();
}

size_t UartTransport::read(uint8_t *destination, size_t limit)
{
    const uintptr_t irq_state = bflb_irq_save();
    observe_rx_errors();
#if defined(BL616CL_AT_RX_DMA) && BL616CL_AT_RX_DMA
    drain_rx_dma();
#endif
    if (rx_overflow_count_ != reported_rx_overflow_count_) {
        bflb_irq_restore(irq_state);
        return 0U;
    }
#if defined(BL616CL_AT_RX_DMA) && BL616CL_AT_RX_DMA
    const size_t count = destination == nullptr ? 0U :
        (limit < dma_available_ ? limit : dma_available_);
    for (size_t i = 0U; i < count; ++i) {
        destination[i] = dma_rx_bytes[dma_read_offset_];
        dma_read_offset_ = (dma_read_offset_ + 1U) % kDmaRxBytes;
    }
    dma_available_ -= count;
#else
    const size_t count = rx_.read(destination, limit);
#endif
    bflb_irq_restore(irq_state);
    return count;
}

size_t UartTransport::write(const uint8_t *source, size_t size)
{
    const size_t written = tx_.write(source, size);
    if (written != size) {
        ++tx_overflow_count_;
    }
    return written;
}

void UartTransport::drain_tx()
{
    if (device_ == nullptr) {
        return;
    }

    uint8_t value = 0U;
    while (bflb_uart_txready(device_) && tx_.pop(value)) {
        (void)bflb_uart_putchar(device_, value);
    }
}

void UartTransport::discard_rx()
{
    const uintptr_t irq_state = bflb_irq_save();
#if defined(BL616CL_AT_RX_DMA) && BL616CL_AT_RX_DMA
    drain_rx_dma();
    dma_available_ = 0U;
    dma_read_offset_ = dma_last_offset_;
#else
    rx_.clear();
#endif
    bflb_irq_restore(irq_state);
}

bool UartTransport::take_rx_overflow()
{
    const uintptr_t irq_state = bflb_irq_save();
    const uint32_t count = rx_overflow_count_;
    bflb_irq_restore(irq_state);

    if (count == reported_rx_overflow_count_) {
        return false;
    }

    reported_rx_overflow_count_ = count;
    return true;
}

uint32_t UartTransport::rx_overflow_count() const
{
    return rx_overflow_count_;
}

uint32_t UartTransport::tx_overflow_count() const
{
    return tx_overflow_count_;
}

UartTransportDiagnostics UartTransport::diagnostics()
{
    UartTransportDiagnostics snapshot = {};
    const uintptr_t irq_state = bflb_irq_save();
    snapshot.initialized = device_ != nullptr;
    observe_rx_errors();
#if defined(BL616CL_AT_RX_DMA) && BL616CL_AT_RX_DMA
    drain_rx_dma();
    snapshot.buffered_rx_bytes = dma_available_;
#else
    snapshot.buffered_rx_bytes = static_cast<uint32_t>(rx_.size());
#endif
    snapshot.received_bytes = received_bytes_;
    snapshot.rx_overflow_count = rx_overflow_count_;
    snapshot.buffered_tx_bytes = static_cast<uint32_t>(tx_.size());
    snapshot.tx_overflow_count = tx_overflow_count_;
    snapshot.rx_error_status_seen = rx_error_status_seen_;
    snapshot.rx_fifo_errors_seen = rx_fifo_errors_seen_;
    bflb_irq_restore(irq_state);
    return snapshot;
}

void UartTransport::irq_handler(int, void *context)
{
    static_cast<UartTransport *>(context)->handle_irq();
}

void UartTransport::handle_irq()
{
    observe_rx_errors();
    const uint32_t status = bflb_uart_get_intstatus(device_);
    if ((status & (UART_INTSTS_RX_FIFO | UART_INTSTS_RTO)) != 0U) {
        if ((status & UART_INTSTS_RTO) != 0U) {
            bflb_uart_int_clear(device_, UART_INTCLR_RTO);
        }
#if defined(BL616CL_AT_RX_DMA) && BL616CL_AT_RX_DMA
        drain_rx_dma();
#else
        drain_rx_fifo();
#endif
    }
}

void UartTransport::observe_rx_errors()
{
    if (device_ == nullptr) return;
    // Read raw status: bflb_uart_get_intstatus hides masked error sources.
    // Lifetime bitsets, not event counts. Never clear errors/FIFO or change IRQ masks.
    const auto base = device_->reg_base;
    rx_error_status_seen_ |= getreg32(base + UART_INT_STS_OFFSET) &
        (UART_INTSTS_RX_FER | UART_INTSTS_PCE | UART_INTSTS_RX_LSE);
    rx_fifo_errors_seen_ |= getreg32(base + UART_FIFO_CONFIG_0_OFFSET) &
        (UART_RX_FIFO_OVERFLOW | UART_RX_FIFO_UNDERFLOW);
}

void UartTransport::drain_rx_fifo()
{
#if !defined(BL616CL_AT_RX_DMA) || !BL616CL_AT_RX_DMA
    while (bflb_uart_rxavailable(device_)) {
        const int value = bflb_uart_getchar(device_);
        if (value < 0) {
            break;
        }
        ++received_bytes_;
        if (!rx_.push(static_cast<uint8_t>(value))) {
            ++rx_overflow_count_;
        }
    }
#endif
}

#if defined(BL616CL_AT_RX_DMA) && BL616CL_AT_RX_DMA

bool UartTransport::begin_rx_dma()
{
    if (dma_rx_owner != nullptr || device_ == nullptr) return false;
    rx_dma_device_ = bflb_device_get_by_name("dma0_ch0");
    if (rx_dma_device_ == nullptr) return false;
    dma_rx_owner = this;
    bflb_dma_channel_config_s cfg = {};
    cfg.direction = DMA_PERIPH_TO_MEMORY;
    cfg.src_req = DMA_REQUEST_UART1_RX;
    cfg.dst_req = DMA_REQUEST_NONE;
    cfg.src_addr_inc = DMA_ADDR_INCREMENT_DISABLE;
    cfg.dst_addr_inc = DMA_ADDR_INCREMENT_ENABLE;
    cfg.src_burst_count = DMA_BURST_INCR1;
    cfg.dst_burst_count = DMA_BURST_INCR1;
    cfg.src_width = DMA_DATA_WIDTH_8BIT;
    cfg.dst_width = DMA_DATA_WIDTH_8BIT;
    bflb_dma_channel_init(rx_dma_device_, &cfg);
    bflb_dma_channel_lli_transfer_s transfer[2] = {};
    for (unsigned i = 0; i < 2; ++i) {
        transfer[i].src_addr = DMA_ADDR_UART1_RDR;
        transfer[i].dst_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(dma_rx_bytes)) + i * kDmaRxBytes / 2;
        transfer[i].nbytes = kDmaRxBytes / 2;
    }
    const int used = bflb_dma_channel_lli_reload(rx_dma_device_, dma_rx_lli, 2, transfer, 2);
    if (used != 2) {
        end_rx_dma();
        return false;
    }
    bflb_dma_channel_lli_link_head(rx_dma_device_, dma_rx_lli, 2);
    bflb_dma_channel_irq_attach(rx_dma_device_, &UartTransport::dma_irq_handler, this);
    (void)bflb_uart_feature_control(device_, UART_CMD_SET_RX_FIFO_THREHOLD, 0U);
    // Capture initial counter before linking UART requests to the running DMA.
    bflb_dma_channel_start(rx_dma_device_);
    dma_last_count_ = bflb_dma_feature_control(rx_dma_device_, DMA_CMD_GET_LLI_COUNT, 0);
    dma_last_offset_ = 0U;
    dma_read_offset_ = 0U;
    dma_available_ = 0U;
    bflb_uart_link_rxdma(device_, true);
    return true;
}

void UartTransport::end_rx_dma()
{
    if (rx_dma_device_ != nullptr) {
        bflb_uart_link_rxdma(device_, false);
        bflb_dma_channel_deinit(rx_dma_device_);
        bflb_dma_channel_irq_detach(rx_dma_device_);
        rx_dma_device_ = nullptr;
    }
    if (dma_rx_owner == this) dma_rx_owner = nullptr;
}

void UartTransport::dma_irq_handler(void *context)
{
    auto *self = static_cast<UartTransport *>(context);
    const uintptr_t state = bflb_irq_save();
    self->observe_rx_errors();
    self->drain_rx_dma();
    bflb_irq_restore(state);
}

void UartTransport::drain_rx_dma()
{
    if (rx_dma_device_ == nullptr) return;
    // DMA runs while CPU interrupts are disabled. Reject a descriptor-boundary
    // race and retry on the next poll/IRQ, without stopping the receiver.
    const uint32_t count = bflb_dma_feature_control(rx_dma_device_, DMA_CMD_GET_LLI_COUNT, 0);
    const uint32_t address = bflb_dma_feature_control(rx_dma_device_, DMA_CMD_GET_LLI_DSTADDR, 0);
    if (count != static_cast<uint32_t>(bflb_dma_feature_control(rx_dma_device_, DMA_CMD_GET_LLI_COUNT, 0))) return;
    const auto base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(dma_rx_bytes));
    if (address < base || address > base + kDmaRxBytes) return;
    const uint32_t offset = (address - base) % kDmaRxBytes;
    const uint32_t added = (offset + kDmaRxBytes - dma_last_offset_) % kDmaRxBytes;
    if (dma_rx_overwritten(dma_last_count_, count, dma_last_offset_, offset) ||
        dma_available_ + added >= kDmaRxBytes) {
        // Notify the existing AT overflow recovery; never parse overwritten data.
        ++rx_overflow_count_;
        dma_last_offset_ = offset;
        dma_last_count_ = count;
        dma_read_offset_ = offset;
        dma_available_ = 0U;
        return;
    }
    received_bytes_ += added;
    dma_available_ += added;
    dma_last_offset_ = offset;
    dma_last_count_ = count;
}
#endif

} // namespace bridge
