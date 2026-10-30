#include "BLEController.h"

extern "C" {
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "stream_buffer.h"
#include "board_overlay.h"
#include "btble_lib_api.h"
#include "btblecontroller_port_uart.h"
}

/* Keep the upstream H4 engine, replacing only its documented UART driver hooks.
 * A single worker owns pending reads and invokes their completion callbacks.
 * FreeRTOS streams each have one producer and one consumer; no Host runs here.
 * Run the worker above the Controller: the upstream ISR callback signals its
 * task before appending the deferred job. That task must not preempt the callback.
 */
static StreamBufferHandle_t host_tx, host_rx;
static QueueHandle_t reads;
static TaskHandle_t transport_task;
static bool running;

struct ReadRequest {
    uint8_t *data;
    uint32_t size;
    void (*done)(void *, uint8_t);
    void *arg;
};

/**
 * @brief Complete queued external-Host H4 reads in the dedicated transport task.
 */
static void transport(void *)
{
    ReadRequest request;
    for (;;) {
        xQueueReceive(reads, &request, portMAX_DELAY);
        size_t count = 0;
        while (count < request.size) {
            count += xStreamBufferReceive(host_tx, request.data + count,
                                          request.size - count, portMAX_DELAY);
        }
        request.done(request.arg, 0);
    }
}

/**
 * @brief Queue an upstream Controller H4 read request and its completion callback.
 */
extern "C" void btble_uart_read(uint8_t *data, uint32_t size,
                                void (*done)(void *, uint8_t), void *arg)
{
    ReadRequest request = {data, size, done, arg};
    xQueueSend(reads, &request, portMAX_DELAY);
}

/**
 * @brief Transfer upstream Controller H4 output into the Host receive stream, then complete
 * the request.
 */
extern "C" void btble_uart_write(const uint8_t *data, uint32_t size,
                                 void (*done)(void *, uint8_t), void *arg)
{
    size_t count = 0;
    while (count < size) {
        count += xStreamBufferSend(host_rx, data + count, size - count, portMAX_DELAY);
    }
    done(arg, 0);
}

/* The in-memory H4 stream has no physical UART to pause or drain. */
/**
 * @brief Provide the upstream flow-on hook; the memory transport has no physical UART.
 */
extern "C" void btble_uart_flow_on(void) {}
/**
 * @brief Accept flow-off for the memory transport, which has no UART hardware to pause.
 */
extern "C" bool btble_uart_flow_off(void) { return true; }
/**
 * @brief Provide the upstream drain hook for the in-memory H4 transport.
 */
extern "C" void btble_uart_finish_transfers(void) {}

/**
 * @brief Initialize the upstream Controller and H4 worker resources; the external Host must
 * then issue HCI Reset.
 */
bool BLEControllerClass::begin()
{
    if (running) {
        return true;
    }
    if (board_radio_init() != 0) {
        return false;
    }
    host_tx = xStreamBufferCreate(1024, 1);
    host_rx = xStreamBufferCreate(1024, 1);
    reads = xQueueCreate(1, sizeof(ReadRequest));
    if (!host_tx || !host_rx || !reads ||
        xTaskCreate(transport, "hci h4", 512, nullptr, configMAX_PRIORITIES - 2,
                    &transport_task) != pdPASS) {
        if (host_tx) vStreamBufferDelete(host_tx);
        if (host_rx) vStreamBufferDelete(host_rx);
        if (reads) vQueueDelete(reads);
        return false;
    }
    btble_controller_init(configMAX_PRIORITIES - 3);
    running = true;
    return true;
}

/**
 * @brief Stop Controller worker resources after the Host has stopped all traffic.
 */
void BLEControllerClass::end()
{
    if (!running) return;
    btble_controller_deinit();
    vTaskDelete(transport_task);
    vQueueDelete(reads);
    vStreamBufferDelete(host_tx);
    vStreamBufferDelete(host_rx);
    running = false;
}

/**
 * @brief Return the currently readable byte count without consuming input.
 */
int BLEControllerClass::available()
{
    return running ? xStreamBufferBytesAvailable(host_rx) : 0;
}

/**
 * @brief Consume available raw H4 bytes without waiting; return the byte/count or -1 for an
 * empty single-byte read.
 */
int BLEControllerClass::read()
{
    uint8_t data;
    return read(&data, 1) ? data : -1;
}

/**
 * @brief Consume available raw H4 bytes without waiting; return the byte/count or -1 for an
 * empty single-byte read.
 */
size_t BLEControllerClass::read(uint8_t *data, size_t size)
{
    return running ? xStreamBufferReceive(host_rx, data, size, 0) : 0;
}

/**
 * @brief Queue raw H4 bytes for the Controller and return the accepted byte count.
 */
size_t BLEControllerClass::write(const uint8_t *data, size_t size)
{
    return running ? xStreamBufferSend(host_tx, data, size, 0) : 0;
}

BLEControllerClass BLEController;
