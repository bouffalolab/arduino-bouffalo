#ifndef UNOR4_BL616CL_UART_TRANSPORT_H
#define UNOR4_BL616CL_UART_TRANSPORT_H

#include "Arduino.h"
#include "bridge_config.h"
#include "byte_ring.h"

namespace bridge {

struct UartTransportDiagnostics {
    bool initialized = false;
    uint32_t received_bytes = 0U;
    uint32_t buffered_rx_bytes = 0U;
    uint32_t rx_overflow_count = 0U;
    uint32_t buffered_tx_bytes = 0U;
    uint32_t tx_overflow_count = 0U;
    uint32_t rx_error_status_seen = 0U;
    uint32_t rx_fifo_errors_seen = 0U;
};

class UartTransport {
public:
    static constexpr size_t kRxCapacity = kAtUartRxBufferBytes;
    static constexpr size_t kTxCapacity = kAtUartTxBufferBytes;

    UartTransport() = default;

    bool begin(HardwareSerial &serial);
    void end();

    size_t read(uint8_t *destination, size_t limit);
    size_t write(const uint8_t *source, size_t size);
    void drain_tx();
    void discard_rx();
    bool take_rx_overflow();
    uint32_t rx_overflow_count() const;
    uint32_t tx_overflow_count() const;
    UartTransportDiagnostics diagnostics();

private:
    static void irq_handler(int irq, void *context);
    void handle_irq();
    void drain_rx_fifo();
    void observe_rx_errors();
#if defined(BL616CL_AT_RX_DMA) && BL616CL_AT_RX_DMA
    static void dma_irq_handler(void *context);
    void drain_rx_dma();
    bool begin_rx_dma();
    void end_rx_dma();
    struct bflb_device_s *rx_dma_device_ = nullptr;
    uint32_t dma_last_count_ = 0U;
    uint32_t dma_last_offset_ = 0U;
    uint32_t dma_read_offset_ = 0U;
    uint32_t dma_available_ = 0U;
#endif

    struct bflb_device_s *device_ = nullptr;
#if !defined(BL616CL_AT_RX_DMA) || !BL616CL_AT_RX_DMA
    ByteRing<kRxCapacity> rx_;
#endif
    ByteRing<kTxCapacity> tx_;
    volatile uint32_t received_bytes_ = 0U;
    volatile uint32_t rx_overflow_count_ = 0U;
    uint32_t reported_rx_overflow_count_ = 0U;
    uint32_t tx_overflow_count_ = 0U;
    volatile uint32_t rx_error_status_seen_ = 0U;
    volatile uint32_t rx_fifo_errors_seen_ = 0U;
};

} // namespace bridge

#endif
