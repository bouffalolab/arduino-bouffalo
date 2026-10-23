/*
 * USB CDC ACM 设备，用底层包中的 CherryUSB。描述符与端点回调参照
 * bouffalo_sdk examples/cherryusb/cherryusb_cli/device_demo/cdc_acm_template.c（Apache-2.0）。
 *
 * CherryUSB 的端点与类请求回调都在 USB 中断里执行。收发缓冲为单生产者单消费者环形缓冲，
 * 下标只增不减，长度为 2 的幂；任务侧启动传输时关中断，与中断互斥。
 * 接收：只在缓冲空余至少一个包时挂接收，否则让主机等待（NAK），读走数据后再挂，不丢数据。
 * 发送：每次传输最多一个包；传输以整包结束且没有后续数据时补零长度包。
 * 1200 bps touch：主机以 1200 波特率打开再关闭端口（DTR 变无效）后，延时让控制请求完成，再复位进下载模式；
 * 延时期间波特率改变或 DTR 重新有效则取消。应用可以关闭 touch。
 * 线路变化回调：中断里对每个 SET_LINE_CODING、SET_CONTROL_LINE_STATE 请求通知一次线路任务，由它调用应用的回调，
 * 回调可以阻塞（如 delay()）；不用定时器任务，因为它优先级最高且 Wi-Fi 协议栈也用它。
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

#define CDC_IN_EP  0x83
#define CDC_OUT_EP 0x04
#define CDC_INT_EP 0x85

#ifdef CONFIG_USB_HS
#define CDC_MAX_MPS 512
#else
#define CDC_MAX_MPS 64
#endif

#define RX_RING_SIZE      1024 /* 2 的幂，至少两个包 */
#define TX_RING_SIZE      1024 /* 2 的幂 */
#define TX_TIMEOUT_MS     100
#define TOUCH_BAUD        1200
#define TOUCH_DELAY_MS    100
#define LINE_TASK_STACK_SIZE 4096 /* 字节 */
#define LINE_TASK_PRIORITY   2    /* 高于 loop 任务（1），请求到达后尽快调用回调 */

#define USB_CONFIG_SIZE (9 + CDC_ACM_DESCRIPTOR_LEN)

static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, USB_VID, USB_PID, 0x0100, 0x01)
};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x02, 0x01, USB_CONFIG_BUS_POWERED, 100),
    CDC_ACM_DESCRIPTOR_INIT(0x00, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP, CDC_MAX_MPS, 0x02)
};

static const uint8_t device_quality_descriptor[] = {
    0x0a, USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
};

static char serial_string[17]; /* 芯片 ID 的十六进制 */

static const char *const string_descriptors[] = {
    (const char[]){ 0x09, 0x04 },
    USB_MANUFACTURER,
    USB_PRODUCT,
    serial_string,
};

static const uint8_t *device_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return device_descriptor;
}

static const uint8_t *config_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return config_descriptor;
}

static const uint8_t *device_quality_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return device_quality_descriptor;
}

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
static volatile size_t rx_head; /* 中断写 */
static volatile size_t rx_tail; /* 读者写 */
static volatile bool rx_armed;

static uint8_t tx_ring[TX_RING_SIZE];
static volatile size_t tx_head; /* 写者写 */
static volatile size_t tx_tail; /* 启动传输时写（关中断或在中断中） */
static volatile size_t tx_inflight; /* 正在传输的字节数 */
static volatile bool tx_busy;

static volatile bool configured;
static volatile bool line_dtr;
static volatile bool line_rts;
static struct cdc_line_coding line_coding = { 115200, 0, 0, 8 };
static volatile bool touch_pending;
static volatile bool touch_enabled = true;
static TaskHandle_t volatile line_task;
static void (*volatile line_callback)(void);

static size_t rx_free(void)
{
    return RX_RING_SIZE - (rx_head - rx_tail);
}

/* 调用方保证不在接收中，且与中断互斥 */
static void rx_arm(void)
{
    rx_armed = true;
    usbd_ep_start_read(0, CDC_OUT_EP, rx_packet, CDC_MAX_MPS);
}

/* 在中断中或关中断时调用 */
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

static void bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    for (uint32_t i = 0; i < nbytes; i++) {
        rx_ring[(rx_head + i) & (RX_RING_SIZE - 1)] = rx_packet[i];
    }
    __atomic_signal_fence(__ATOMIC_RELEASE); /* 先写数据再发布 head */
    rx_head += nbytes;
    rx_armed = false;
    if (rx_free() >= CDC_MAX_MPS) {
        rx_arm();
    }
}

static void bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    (void)nbytes;
    bool full_packet = tx_inflight == CDC_MAX_MPS;
    tx_inflight = 0;
    tx_busy = false;
    if (full_packet && tx_head == tx_tail) {
        tx_busy = true; /* 零长度包结束本次传输 */
        usbd_ep_start_write(0, CDC_IN_EP, NULL, 0);
        return;
    }
    tx_kick();
}

static void event_handler(uint8_t busid, uint8_t event)
{
    (void)busid;
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
            tx_tail = tx_head; /* 未发出的数据不再有接收方 */
            break;
        default:
            break;
    }
}

static void touch_reboot(void *arg, uint32_t unused)
{
    (void)arg;
    (void)unused;
    vTaskDelay(pdMS_TO_TICKS(TOUCH_DELAY_MS));
    if (touch_pending) {
        isp_boot_reboot();
    }
}

/* 在中断中调用 */
static void touch_check(void)
{
    bool touch = touch_enabled && line_coding.dwDTERate == TOUCH_BAUD && !line_dtr;
    if (!touch) {
        touch_pending = false;
        return;
    }
    if (!touch_pending) {
        BaseType_t woken = pdFALSE;
        /* 定时器队列满时放弃本次 touch，下次 touch 仍能触发 */
        touch_pending = xTimerPendFunctionCallFromISR(touch_reboot, NULL, 0, &woken) == pdPASS;
        portYIELD_FROM_ISR(woken);
    }
}

/* 在中断中调用，每个线路请求一次 */
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

void usbd_cdc_acm_set_line_coding(uint8_t busid, uint8_t intf, struct cdc_line_coding *coding)
{
    (void)busid;
    (void)intf;
    memcpy(&line_coding, coding, sizeof(line_coding));
    touch_check();
    line_notify();
}

void usbd_cdc_acm_get_line_coding(uint8_t busid, uint8_t intf, struct cdc_line_coding *coding)
{
    (void)busid;
    (void)intf;
    memcpy(coding, &line_coding, sizeof(line_coding));
}

void usbd_cdc_acm_set_dtr(uint8_t busid, uint8_t intf, bool dtr)
{
    (void)busid;
    (void)intf;
    line_dtr = dtr;
    touch_check();
}

void usbd_cdc_acm_set_rts(uint8_t busid, uint8_t intf, bool rts)
{
    (void)busid;
    (void)intf;
    line_rts = rts;
    line_notify(); /* CherryUSB 对每个 SET_CONTROL_LINE_STATE 先调 set_dtr 再调 set_rts，在这里通知一次 */
}

static struct usbd_endpoint out_ep = { .ep_addr = CDC_OUT_EP, .ep_cb = bulk_out };
static struct usbd_endpoint in_ep = { .ep_addr = CDC_IN_EP, .ep_cb = bulk_in };
static struct usbd_interface intf0;
static struct usbd_interface intf1;

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
    usbd_initialize(0, 0, event_handler);
}

void usb_cdc_port_set_touch_reset(bool enable)
{
    touch_enabled = enable;
    if (!enable) {
        touch_pending = false; /* 取消已安排、尚未执行的 touch */
    }
}

static void line_task_main(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdFALSE, portMAX_DELAY); /* 每次取一个请求 */
        void (*callback)(void) = line_callback;
        if (callback != NULL) {
            callback();
        }
    }
}

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

bool usb_cdc_port_connected(void)
{
    return configured && line_dtr;
}

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

size_t usb_cdc_port_rx_count(void)
{
    return rx_head - rx_tail;
}

int usb_cdc_port_peek(void)
{
    if (rx_head == rx_tail) {
        return -1;
    }
    __atomic_signal_fence(__ATOMIC_ACQUIRE); /* 读到 head 之后再读数据 */
    return rx_ring[rx_tail & (RX_RING_SIZE - 1)];
}

int usb_cdc_port_read(void)
{
    if (rx_head == rx_tail) {
        return -1;
    }
    __atomic_signal_fence(__ATOMIC_ACQUIRE);
    uint8_t c = rx_ring[rx_tail & (RX_RING_SIZE - 1)];
    __atomic_signal_fence(__ATOMIC_RELEASE); /* 读完数据再让出这个位置 */
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

size_t usb_cdc_port_tx_space(void)
{
    return TX_RING_SIZE - (tx_head - tx_tail);
}

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
