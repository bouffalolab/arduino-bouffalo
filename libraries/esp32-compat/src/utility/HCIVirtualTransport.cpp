#include "utility/HCIVirtualTransport.h"
#include "utility/hci_transport_internal.h"
#include "ble/ble_hci_port.h"

#include <Arduino.h>
#include <FreeRTOS.h>
#include <task.h>

#include <string.h>

// The BLE controller's RW task polls xRwmainQueue with a timeout and runs
// rwip_schedule() when woken.  The H4TL layer registers its RX djob from the
// eif read callback (co_djob_isr_reg), but nothing posts the queue when the
// transport is our AT channel instead of a UART ISR, so the djob never runs
// and the stack never re-arms its read.  Post the wake message ourselves.
extern "C" void *xRwmainQueue;
extern "C" void *rw_main_task_hdl;
extern "C" int btblecontroller_queue_send(void *q, void *msg, uint32_t size,
                                          uint32_t timeout);
extern "C" uint32_t rwip_prevent_sleep_get(void);
extern "C" void rwip_prevent_sleep_clear(uint32_t value);
extern "C" void btble_ke_event_schedule(void);

// The H4TL layer defers its RX processing through the RW djob machinery,
// which never runs on this transport (no UART ISR drives the wake chain
// reliably).  Parse the HCI byte stream ourselves and feed the controller
// through the HCI transport layer entry points directly; controller-to-host
// events already flow out through h4tl_write -> the eif write (our port).
extern "C" void hci_tl_cmd_received(uint8_t tl_type, uint16_t opcode,
                                    uint8_t length, uint8_t *payload);
// Side-effect preamble required by hci_tl_cmd_received(): sets
// hci_env.p_cmd_desc from the opcode (the H4TL does this via its header
// handling before delivering the command).
extern "C" uint8_t hci_tl_cmd_get_max_param_size(uint16_t opcode);
extern "C" uint8_t *hci_tl_acl_tx_data_alloc(uint8_t tl_type,
                                             uint16_t hdl_flags,
                                             uint16_t datalen);
extern "C" void hci_tl_acl_tx_data_received(uint8_t tl_type,
                                            uint16_t hdl_flags,
                                            uint16_t datalen,
                                            uint8_t *payload);

static uint32_t wake_send_count = 0;
static uint32_t wake_send_result = 0;

static void wake_rw_task(void)
{
    static uint32_t wake_msg[2] = {0, 0};
    wake_send_count++;
    if (xRwmainQueue != nullptr) {
        wake_send_result =
            (uint32_t)btblecontroller_queue_send(xRwmainQueue, wake_msg,
                                                 sizeof(wake_msg), 0);
    }
    // The deferred jobs (H4TL tx_done / rx) never run through the ROM's
    // wake chain on this transport; run the scheduler directly so the djob
    // queues drain in the caller's context.
    btble_ke_event_schedule();
}

// Two byte streams cross this boundary:
//
//   h2c: host -> controller.  AT+HCI_WRITE bytes land here and are consumed
//        by the armed read of the btblecontroller port (deliver()).
//   c2h: controller -> host.  The BLE port pushes controller events here;
//        AT+HCI_READ polls them via available()/read().
//
// The queues are deliberately modest: advertising bring-up only needs small
// HCI command/event packets.  Flow control for sustained ACL traffic is a
// follow-up.

namespace {

constexpr size_t kQueueSize = 2048;

uint8_t h2c_buf[kQueueSize];
uint8_t c2h_buf[kQueueSize];
size_t h2c_head = 0, h2c_tail = 0;   // head == tail means empty
size_t c2h_head = 0, c2h_tail = 0;

struct ArmedRead {
    uint8_t *buf;
    uint32_t size;
    uint32_t idx;
    void (*cb)(void *, uint8_t);
    void *dummy;
};

ArmedRead armed = { nullptr, 0, 0, nullptr, nullptr };

uint32_t read_arm_count = 0;
uint32_t cb_fired_count = 0;
uint32_t ctrl_write_count = 0;

bool h2c_empty()
{
    return h2c_head == h2c_tail;
}

size_t h2c_count()
{
    return (h2c_tail - h2c_head + kQueueSize) % kQueueSize;
}

}  // namespace
#define HCI_TL_H4 0U

static uint8_t hci_payload[256];
static uint8_t rx_state = 0;   // 0=type, 1=cmd hdr, 2=cmd payload, 3=acl hdr, 4=acl payload
static uint8_t rx_hdr[4];
static uint8_t rx_idx = 0;
static uint16_t rx_pkt_len = 0;
static uint16_t rx_hdl_flags = 0;

static void hci_parser_feed(uint8_t byte)
{
    switch (rx_state) {
        case 0: // packet type
            rx_idx = 0;
            if (byte == 0x01) {
                rx_state = 1;
            } else if (byte == 0x02) {
                rx_state = 3;
            }
            // anything else: stay in sync hunt
            break;
        case 1: // command header: opcode(2 LE) + len(1)
            rx_hdr[rx_idx++] = byte;
            if (rx_idx == 3) {
                uint16_t opcode = (uint16_t)(rx_hdr[0] | (rx_hdr[1] << 8));
                rx_pkt_len = rx_hdr[2];
                rx_idx = 0;
                hci_tl_cmd_get_max_param_size(opcode);
                if (rx_pkt_len == 0) {
                    hci_tl_cmd_received(HCI_TL_H4, opcode, 0, NULL);
                    rx_state = 0;
                } else {
                    rx_state = 2;
                }
            }
            break;
        case 2: // command payload
            hci_payload[rx_idx++] = byte;
            if (rx_idx == rx_pkt_len) {
                hci_tl_cmd_received(HCI_TL_H4,
                                    (uint16_t)(rx_hdr[0] | (rx_hdr[1] << 8)),
                                    (uint8_t)rx_pkt_len, hci_payload);
                rx_state = 0;
            }
            break;
        case 3: // ACL header: hdl_flags(2 LE) + datalen(2 LE)
            rx_hdr[rx_idx++] = byte;
            if (rx_idx == 4) {
                rx_hdl_flags = (uint16_t)(rx_hdr[0] | (rx_hdr[1] << 8));
                rx_pkt_len = (uint16_t)(rx_hdr[2] | (rx_hdr[3] << 8));
                rx_idx = 0;
                if (rx_pkt_len == 0) {
                    hci_tl_acl_tx_data_received(HCI_TL_H4, rx_hdl_flags, 0,
                                                NULL);
                    rx_state = 0;
                } else {
                    rx_state = 4;
                }
            }
            break;
        case 4: // ACL payload
            hci_payload[rx_idx++] = byte;
            if (rx_idx == rx_pkt_len) {
                uint8_t *dst = hci_tl_acl_tx_data_alloc(HCI_TL_H4,
                                                        rx_hdl_flags,
                                                        rx_pkt_len);
                if (dst != NULL) {
                    memcpy(dst, hci_payload, rx_pkt_len);
                    hci_tl_acl_tx_data_received(HCI_TL_H4, rx_hdl_flags,
                                                rx_pkt_len, dst);
                }
                rx_state = 0;
            }
            break;
        default:
            rx_state = 0;
            break;
    }
}

static void hci_parser_pump(void)
{
    taskENTER_CRITICAL();
    while (h2c_head != h2c_tail) {
        uint8_t byte = h2c_buf[h2c_head];
        h2c_head = (h2c_head + 1) % kQueueSize;
        taskEXIT_CRITICAL();
        hci_parser_feed(byte);
        taskENTER_CRITICAL();
    }
    taskEXIT_CRITICAL();

    // Experiment: the bypassed H4TL leaves RW_TL_1_RX_ONGOING (0x400) set,
    // and the LL appears to gate advertising activity on the HCI RX state.
    // Clear the stuck bit after delivering host commands.
    rwip_prevent_sleep_clear(0x400);
}


size_t hci_h2c_push(const uint8_t *buf, size_t size)
{
    taskENTER_CRITICAL();
    size_t accepted = 0;
    if (h2c_count() + size <= kQueueSize) {
        for (size_t i = 0; i < size; i++) {
            h2c_buf[h2c_tail] = buf[i];
            h2c_tail = (h2c_tail + 1) % kQueueSize;
        }
        accepted = size;
    }
    taskEXIT_CRITICAL();
    return accepted;
}

size_t hci_c2h_push(const uint8_t *buf, size_t size)
{
    taskENTER_CRITICAL();
    size_t used = (c2h_tail - c2h_head + kQueueSize) % kQueueSize;
    if (used + size <= kQueueSize) {
        for (size_t i = 0; i < size; i++) {
            c2h_buf[c2h_tail] = buf[i];
            c2h_tail = (c2h_tail + 1) % kQueueSize;
        }
    }
    // On overflow drop the whole packet instead of half of it: a torn HCI
    // packet in the stream would corrupt framing for the host.
    taskEXIT_CRITICAL();
    return size;
}

int hci_c2h_available()
{
    taskENTER_CRITICAL();
    size_t used = (c2h_tail - c2h_head + kQueueSize) % kQueueSize;
    taskEXIT_CRITICAL();
    return static_cast<int>(used);
}

int hci_c2h_read()
{
    taskENTER_CRITICAL();
    int ret = -1;
    if (c2h_head != c2h_tail) {
        ret = c2h_buf[c2h_head];
        c2h_head = (c2h_head + 1) % kQueueSize;
    }
    taskEXIT_CRITICAL();
    return ret;
}

void hci_transport_reset()
{
    taskENTER_CRITICAL();
    h2c_head = h2c_tail = 0;
    c2h_head = c2h_tail = 0;
    armed.buf = nullptr;
    armed.size = 0;
    armed.idx = 0;
    armed.cb = nullptr;
    armed.dummy = nullptr;
    taskEXIT_CRITICAL();
}

void hci_port_arm_read(uint8_t *buf, uint32_t size,
                       void (*cb)(void *, uint8_t), void *dummy)
{
    taskENTER_CRITICAL();
    armed.buf = buf;
    armed.size = size;
    armed.idx = 0;
    armed.cb = cb;
    armed.dummy = dummy;
    read_arm_count++;
    taskEXIT_CRITICAL();
}

// Satisfy the armed read (if any) from the h2c queue.  The read callback runs
// outside the critical section, from the task that pushed the data, which is
// strictly less restrictive than the stock port's ISR context.
void hci_port_deliver()
{
    // The h2c stream is consumed by hci_parser_pump() instead: the H4TL
    // layer's RX processing is deferred through the RW djob machinery, which
    // never runs on this transport, so feeding its armed read would stall
    // after the first byte.  The armed read stays pending (harmless); we
    // only wake the RW task so any controller-side work gets scheduled.
    wake_rw_task();
}

void hci_transport_state(uint32_t *out)
{
    taskENTER_CRITICAL();
    out[0] = (armed.buf != nullptr) ? 1U : 0U;
    out[1] = (uint32_t)h2c_count();
    out[2] = (uint32_t)((c2h_tail - c2h_head + kQueueSize) % kQueueSize);
    out[3] = read_arm_count;
    out[4] = cb_fired_count;
    out[5] = ctrl_write_count;
    out[6] = wake_send_count;
    out[7] = wake_send_result;
    out[8] = (rw_main_task_hdl != nullptr) ? 1U : 0U;
    out[9] = rwip_prevent_sleep_get();
    out[10] = (uint32_t)eTaskGetState((TaskHandle_t)rw_main_task_hdl);
    out[11] = (uint32_t)uxQueueMessagesWaiting(
        (QueueHandle_t)xRwmainQueue);
    taskEXIT_CRITICAL();
}

void hci_transport_note_write(void)
{
    taskENTER_CRITICAL();
    ctrl_write_count++;
    taskEXIT_CRITICAL();
}

bool HCIVirtualTransportClass::begin()
{
    hci_transport_reset();
    return ble_controller_start();
}

void HCIVirtualTransportClass::end()
{
    // Keep the controller running; only discard in-flight bytes.
    hci_transport_reset();
}

void HCIVirtualTransportClass::wait(int timeoutMs)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeoutMs);
    while (hci_c2h_available() == 0 &&
           xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

int HCIVirtualTransportClass::available()
{
    return hci_c2h_available();
}

int HCIVirtualTransportClass::read()
{
    return hci_c2h_read();
}

size_t HCIVirtualTransportClass::write(const uint8_t *buffer, size_t size)
{
    size_t accepted = hci_h2c_push(buffer, size);
    if (accepted > 0) {
        hci_parser_pump();
        wake_rw_task();
    }
    return accepted;
}
