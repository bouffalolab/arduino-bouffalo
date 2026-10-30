/*
 * USB CDC ACM device built on package CherryUSB, with descriptors and callbacks based on the
 * Apache-2.0 SDK cdc_acm_template.c example. Endpoint/class callbacks run in USB interrupt
 * context. Power-of-two SPSC rings use monotonic indices; task-side transfer setup masks
 * interrupts. RX is armed only with at least one packet of free space, otherwise NAK until the
 * reader frees space. TX sends one packet at a time and appends a zero-length packet when needed.
 * On 1200 bps close, defer reset until the control request completes; a baud change or DTR
 * reassertion cancels it, and applications may disable touch. Each line-control request wakes a
 * separate line task so callbacks may block. Do not use the highest-priority timer task, which is
 * also used by Wi-Fi.
 */
#if defined(USB_VID)

#if !defined(USB_PID) || !defined(USB_MANUFACTURER) || !defined(USB_PRODUCT)
#error "USB_VID 需要同时定义 USB_PID、USB_MANUFACTURER、USB_PRODUCT（boards.txt 的 build.usb_flags）"
#endif

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

#include "bflb_efuse.h"
#include "usbd_core.h"
#include "usbd_cdc_acm.h"

#include "isp_boot.h"
#include "usb_cdc_port.h"
#include "usb_hid_port.h"

#define CDC_IN_EP  0x83
#define CDC_OUT_EP 0x04
#define CDC_INT_EP 0x85

#ifdef CONFIG_USB_HS
#define CDC_MAX_MPS 512
#else
#define CDC_MAX_MPS 64
#endif

#define RX_RING_SIZE      1024 /* Power of two, at least two packets. */
#define TX_RING_SIZE      1024 /* Power of two. */
#define TX_TIMEOUT_MS     100
#define TOUCH_BAUD        1200
#define TOUCH_DELAY_MS    100
#define LINE_TASK_STACK_SIZE 4096 /* Bytes. */
#define LINE_TASK_PRIORITY   2    /* Above loop priority (1), to deliver line requests promptly. */

#define USB_CONFIG_SIZE (9 + CDC_ACM_DESCRIPTOR_LEN)

static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, USB_VID, USB_PID, 0x0100, 0x01)
};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x02, 0x01, USB_CONFIG_BUS_POWERED, 100),
    CDC_ACM_DESCRIPTOR_INIT(0x00, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP, CDC_MAX_MPS, 0x02)
};

static uint8_t composite_descriptor[USB_CONFIG_SIZE + 32];
static const uint8_t *active_config_descriptor = config_descriptor;

static const uint8_t device_quality_descriptor[] = {
    0x0a, USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
};

static char serial_string[17]; /* Chip ID encoded as hexadecimal. */

static const char *const string_descriptors[] = {
    (const char[]){ 0x09, 0x04 },
    USB_MANUFACTURER,
    USB_PRODUCT,
    serial_string,
};

/**
 * @brief Return the USB device descriptor for the requested bus speed.
 */
static const uint8_t *device_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return device_descriptor;
}

/**
 * @brief Return the USB configuration descriptor for the requested bus speed.
 */
static const uint8_t *config_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return active_config_descriptor;
}

/**
 * @brief Report the absence of a USB device-qualifier descriptor.
 */
static const uint8_t *device_quality_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return device_quality_descriptor;
}

/**
 * @brief Return the requested USB string descriptor, including the chip-derived serial number.
 */
static const char *string_descriptor_callback(uint8_t speed, uint8_t index)
{
    (void)speed;
    return index < sizeof(string_descriptors) / sizeof(string_descriptors[0]) ? string_descriptors[index] : NULL;
}

static const struct usb_descriptor cdc_descriptor = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
};

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t rx_packet[CDC_MAX_MPS];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t tx_packet[CDC_MAX_MPS];

static uint8_t rx_ring[RX_RING_SIZE];
static volatile size_t rx_head; /* Written by the interrupt handler. */
static volatile size_t rx_tail; /* Written by the reader. */
static volatile bool rx_armed;

static uint8_t tx_ring[TX_RING_SIZE];
static volatile size_t tx_head; /* Written by the producer. */
static volatile size_t tx_tail; /* Written when starting a transfer, in interrupt context or with interrupts masked. */
static volatile size_t tx_inflight; /* Number of bytes currently in flight. */
static volatile bool tx_busy;

static volatile bool configured;
static volatile bool line_dtr;
static volatile bool line_rts;
static struct cdc_line_coding line_coding = { 115200, 0, 0, 8 };
static volatile bool touch_pending;
static volatile bool touch_enabled = true;
static TaskHandle_t volatile line_task;
static void (*volatile line_callback)(void);

/**
 * @brief Return unused capacity in the USB CDC receive ring.
 */
static size_t rx_free(void)
{
    return RX_RING_SIZE - (rx_head - rx_tail);
}

/**
 * @brief The caller must ensure RX is idle and exclude the interrupt handler.
 */
static void rx_arm(void)
{
    rx_armed = true;
    usbd_ep_start_read(0, CDC_OUT_EP, rx_packet, CDC_MAX_MPS);
}

/**
 * @brief Call in interrupt context or with interrupts masked.
 */
static void tx_kick(void)
{
    if (tx_busy || !configured) {
        return;
    }
    size_t n = tx_head - tx_tail;
    if (n == 0) {
        return;
    }
    if (n > CDC_MAX_MPS) {
        n = CDC_MAX_MPS;
    }
    for (size_t i = 0; i < n; i++) {
        tx_packet[i] = tx_ring[(tx_tail + i) & (TX_RING_SIZE - 1)];
    }
    tx_tail += n;
    tx_inflight = n;
    tx_busy = true;
    usbd_ep_start_write(0, CDC_IN_EP, tx_packet, n);
}

/**
 * @brief Publish a completed USB OUT packet and rearm reception when space permits.
 */
static void bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    for (uint32_t i = 0; i < nbytes; i++) {
        rx_ring[(rx_head + i) & (RX_RING_SIZE - 1)] = rx_packet[i];
    }
    __atomic_signal_fence(__ATOMIC_RELEASE); /* Store data before publishing head. */
    rx_head += nbytes;
    rx_armed = false;
    if (rx_free() >= CDC_MAX_MPS) {
        rx_arm();
    }
}

/**
 * @brief Advance completed USB IN output and schedule the next packet.
 */
static void bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    (void)nbytes;
    bool full_packet = tx_inflight == CDC_MAX_MPS;
    tx_inflight = 0;
    tx_busy = false;
    if (full_packet && tx_head == tx_tail) {
        tx_busy = true; /* A zero-length packet terminates this transfer. */
        usbd_ep_start_write(0, CDC_IN_EP, NULL, 0);
        return;
    }
    tx_kick();
}

/**
 * @brief Update CDC state and transfers for USB device lifecycle events.
 */
static void event_handler(uint8_t busid, uint8_t event)
{
    (void)busid;
    usb_hid_port_event(event);
    switch (event) {
        case USBD_EVENT_CONFIGURED:
            configured = true;
            tx_busy = false;
            tx_inflight = 0;
            if (!rx_armed && rx_free() >= CDC_MAX_MPS) {
                rx_arm();
            }
            break;
        case USBD_EVENT_RESET:
        case USBD_EVENT_DISCONNECTED:
            configured = false;
            line_dtr = false;
            rx_armed = false;
            tx_busy = false;
            tx_inflight = 0;
            tx_tail = tx_head; /* There is no longer a receiver for queued output. */
            break;
        default:
            break;
    }
}

/**
 * @brief Perform a deferred ROM reset only if the 1200 bps close request is still valid.
 */
static void touch_reboot(void *arg, uint32_t unused)
{
    (void)arg;
    (void)unused;
    vTaskDelay(pdMS_TO_TICKS(TOUCH_DELAY_MS));
    if (touch_pending) {
        isp_boot_reboot();
    }
}

/**
 * @brief Called from interrupt context.
 */
static void touch_check(void)
{
    bool touch = touch_enabled && line_coding.dwDTERate == TOUCH_BAUD && !line_dtr;
    if (!touch) {
        touch_pending = false;
        return;
    }
    if (!touch_pending) {
        BaseType_t woken = pdFALSE;
        /* If the timer queue is full, abandon this touch request; a later touch can still trigger reset. */
        touch_pending = xTimerPendFunctionCallFromISR(touch_reboot, NULL, 0, &woken) == pdPASS;
        portYIELD_FROM_ISR(woken);
    }
}

/**
 * @brief Called once per line request from interrupt context.
 */
static void line_notify(void)
{
    TaskHandle_t task = line_task;
    if (task == NULL) {
        return;
    }
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(task, &woken);
    portYIELD_FROM_ISR(woken);
}

/**
 * @brief Store host line coding and notify the line task from the USB class callback.
 */
void usbd_cdc_acm_set_line_coding(uint8_t busid, uint8_t intf, struct cdc_line_coding *coding)
{
    (void)busid;
    (void)intf;
    memcpy(&line_coding, coding, sizeof(line_coding));
    touch_check();
    line_notify();
}

/**
 * @brief Copy the current line coding into the USB control-request response.
 */
void usbd_cdc_acm_get_line_coding(uint8_t busid, uint8_t intf, struct cdc_line_coding *coding)
{
    (void)busid;
    (void)intf;
    memcpy(coding, &line_coding, sizeof(line_coding));
}

/**
 * @brief Store the host DTR signal and evaluate touch-reset eligibility.
 */
void usbd_cdc_acm_set_dtr(uint8_t busid, uint8_t intf, bool dtr)
{
    (void)busid;
    (void)intf;
    line_dtr = dtr;
    touch_check();
}

/**
 * @brief Store RTS and notify once after the paired DTR/RTS class callbacks.
 */
void usbd_cdc_acm_set_rts(uint8_t busid, uint8_t intf, bool rts)
{
    (void)busid;
    (void)intf;
    line_rts = rts;
    line_notify(); /* CherryUSB calls set_dtr then set_rts for each SET_CONTROL_LINE_STATE request; notify once here. */
}

static struct usbd_endpoint out_ep = { .ep_addr = CDC_OUT_EP, .ep_cb = bulk_out };
static struct usbd_endpoint in_ep = { .ep_addr = CDC_IN_EP, .ep_cb = bulk_in };
static struct usbd_interface intf0;
static struct usbd_interface intf1;

/**
 * @brief Register selected USB interfaces and start the device after scheduler startup.
 */
void usb_cdc_port_init(void)
{
    uint8_t id[8] = { 0 };
    bflb_efuse_get_chipid(id);
    for (size_t i = 0; i < sizeof(id); i++) {
        snprintf(&serial_string[i * 2], 3, "%02X", id[i]);
    }

    usbd_desc_register(0, &cdc_descriptor);
    usbd_add_interface(0, usbd_cdc_acm_init_intf(0, &intf0));
    usbd_add_interface(0, usbd_cdc_acm_init_intf(0, &intf1));
    usbd_add_endpoint(0, &out_ep);
    usbd_add_endpoint(0, &in_ep);
    const uint8_t *hid = usb_hid_port_init();
    if (hid != NULL) {
        memcpy(composite_descriptor, config_descriptor, USB_CONFIG_SIZE);
        memcpy(composite_descriptor + USB_CONFIG_SIZE, hid, 32);
        composite_descriptor[2] = sizeof(composite_descriptor) & 0xff;
        composite_descriptor[3] = sizeof(composite_descriptor) >> 8;
        composite_descriptor[4] = 3;
        active_config_descriptor = composite_descriptor;
    }
    usbd_initialize(0, 0, event_handler);
}

/**
 * @brief Enable touch-reset handling or cancel an outstanding request when disabling it.
 */
void usb_cdc_port_set_touch_reset(bool enable)
{
    touch_enabled = enable;
    if (!enable) {
        touch_pending = false; /* Cancel a scheduled touch that has not executed yet. */
    }
}

/**
 * @brief Deliver pending host line requests serially in the dedicated callback task.
 */
static void line_task_main(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdFALSE, portMAX_DELAY); /* Consume one pending request at a time. */
        void (*callback)(void) = line_callback;
        if (callback != NULL) {
            callback();
        }
    }
}

/**
 * @brief Register a line callback and create its task on first use; NULL unregisters it.
 */
bool usb_cdc_port_on_line_change(void (*callback)(void))
{
    line_callback = callback;
    if (callback == NULL || line_task != NULL) {
        return true;
    }
    TaskHandle_t task;
    if (xTaskCreate(line_task_main, "usb_line", LINE_TASK_STACK_SIZE / sizeof(StackType_t), NULL,
                    LINE_TASK_PRIORITY, &task) != pdPASS) {
        line_callback = NULL;
        return false;
    }
    line_task = task;
    return true;
}

/**
 * @brief Return whether USB is configured and the host has asserted DTR.
 */
bool usb_cdc_port_connected(void)
{
    return configured && line_dtr;
}

/**
 * @brief Copy the host CDC settings and control signals into the caller's structure.
 */
void usb_cdc_port_get_line(struct usb_cdc_port_line *line)
{
    taskENTER_CRITICAL();
    line->baud = line_coding.dwDTERate;
    line->stop_bits = line_coding.bCharFormat;
    line->parity = line_coding.bParityType;
    line->data_bits = line_coding.bDataBits;
    line->dtr = line_dtr;
    line->rts = line_rts;
    taskEXIT_CRITICAL();
}

/**
 * @brief Return the number of unread bytes in the CDC RX ring.
 */
size_t usb_cdc_port_rx_count(void)
{
    return rx_head - rx_tail;
}

/**
 * @brief Return the next CDC input byte without consuming it, or -1 if empty.
 */
int usb_cdc_port_peek(void)
{
    if (rx_head == rx_tail) {
        return -1;
    }
    __atomic_signal_fence(__ATOMIC_ACQUIRE); /* Read head before accessing the corresponding data. */
    return rx_ring[rx_tail & (RX_RING_SIZE - 1)];
}

/**
 * @brief Consume the next CDC input byte, or return -1 if empty.
 */
int usb_cdc_port_read(void)
{
    if (rx_head == rx_tail) {
        return -1;
    }
    __atomic_signal_fence(__ATOMIC_ACQUIRE);
    uint8_t c = rx_ring[rx_tail & (RX_RING_SIZE - 1)];
    __atomic_signal_fence(__ATOMIC_RELEASE); /* Release the slot after reading its data. */
    rx_tail = rx_tail + 1;
    if (!rx_armed && configured && rx_free() >= CDC_MAX_MPS) {
        taskENTER_CRITICAL();
        if (!rx_armed && configured && rx_free() >= CDC_MAX_MPS) {
            rx_arm();
        }
        taskEXIT_CRITICAL();
    }
    return c;
}

/**
 * @brief Return currently available CDC transmit-ring capacity.
 */
size_t usb_cdc_port_tx_space(void)
{
    return TX_RING_SIZE - (tx_head - tx_tail);
}

/**
 * @brief Queue CDC bytes with bounded progress waits; return the accepted byte count.
 */
size_t usb_cdc_port_write(const uint8_t *buf, size_t len)
{
    size_t done = 0;
    TickType_t progress = xTaskGetTickCount();

    while (done < len && usb_cdc_port_connected()) {
        size_t space = usb_cdc_port_tx_space();
        if (space == 0) {
            if (xTaskGetTickCount() - progress > pdMS_TO_TICKS(TX_TIMEOUT_MS)) {
                break;
            }
            vTaskDelay(1);
            continue;
        }
        size_t n = len - done < space ? len - done : space;
        for (size_t i = 0; i < n; i++) {
            tx_ring[(tx_head + i) & (TX_RING_SIZE - 1)] = buf[done + i];
        }
        __atomic_signal_fence(__ATOMIC_RELEASE);
        tx_head = tx_head + n;
        done += n;
        progress = xTaskGetTickCount();
        taskENTER_CRITICAL();
        tx_kick();
        taskEXIT_CRITICAL();
    }
    return done;
}

/**
 * @brief Wait for queued CDC output until drained, disconnected or stalled for 100 ms.
 */
void usb_cdc_port_flush(void)
{
    TickType_t progress = xTaskGetTickCount();
    size_t last = tx_tail;

    while ((tx_busy || tx_head != tx_tail) && usb_cdc_port_connected()) {
        if (tx_tail != last) {
            last = tx_tail;
            progress = xTaskGetTickCount();
        } else if (xTaskGetTickCount() - progress > pdMS_TO_TICKS(TX_TIMEOUT_MS)) {
            break;
        }
        vTaskDelay(1);
    }
}

#endif /* USB_VID */
