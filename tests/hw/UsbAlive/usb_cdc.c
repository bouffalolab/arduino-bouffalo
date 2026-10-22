/*
 * 实板诊断用的最小 USB CDC ACM 设备，直接使用底层包中的 CherryUSB。
 * 平台还没有 SerialUSB；UNO R4 WiFi 上 BL616CL 的 UART0 没有引到可接的位置，
 * 这是目前读取 sketch 输出的唯一通道。描述符与端点回调参照
 * bouffalo_sdk examples/cherryusb/cherryusb_cli/device_demo/cdc_acm_template.c（Apache-2.0）。
 * 主机未读取时发送不阻塞，直接丢弃这一行。
 */
#include <string.h>

#include "usbd_core.h"
#include "usbd_cdc_acm.h"

#include "usb_cdc.h"

#define CDC_IN_EP  0x83
#define CDC_OUT_EP 0x04
#define CDC_INT_EP 0x85

/* 与 ROM 下载模式（ffff:ffff）区分 */
#define USBD_VID 0xFFFF
#define USBD_PID 0x4142

#define USB_CONFIG_SIZE (9 + CDC_ACM_DESCRIPTOR_LEN)
#ifdef CONFIG_USB_HS
#define CDC_MAX_MPS 512
#else
#define CDC_MAX_MPS 64
#endif

static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, USBD_VID, USBD_PID, 0x0100, 0x01)
};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x02, 0x01, USB_CONFIG_BUS_POWERED, 100),
    CDC_ACM_DESCRIPTOR_INIT(0x00, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP, CDC_MAX_MPS, 0x02)
};

static const uint8_t device_quality_descriptor[] = {
    0x0a, USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
};

static const char *string_descriptors[] = {
    (const char[]){ 0x09, 0x04 },
    "arduino-bl",
    "UsbAlive",
    "0001",
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

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t rx_buf[CDC_MAX_MPS];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t tx_buf[256];
static volatile bool configured;
static volatile bool tx_busy;

static void event_handler(uint8_t busid, uint8_t event)
{
    switch (event) {
        case USBD_EVENT_CONFIGURED:
            configured = true;
            tx_busy = false;
            usbd_ep_start_read(busid, CDC_OUT_EP, rx_buf, sizeof(rx_buf));
            break;
        case USBD_EVENT_RESET:
        case USBD_EVENT_DISCONNECTED:
            configured = false;
            tx_busy = false;
            break;
        default:
            break;
    }
}

/* 收到的数据丢弃，只重新挂接收 */
static void bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)ep;
    (void)nbytes;
    usbd_ep_start_read(busid, CDC_OUT_EP, rx_buf, sizeof(rx_buf));
}

static void bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)ep;
    if (nbytes != 0 && nbytes % CDC_MAX_MPS == 0) {
        usbd_ep_start_write(busid, CDC_IN_EP, NULL, 0); /* 整包结尾补零长度包 */
    } else {
        tx_busy = false;
    }
}

static struct usbd_endpoint out_ep = { .ep_addr = CDC_OUT_EP, .ep_cb = bulk_out };
static struct usbd_endpoint in_ep = { .ep_addr = CDC_IN_EP, .ep_cb = bulk_in };
static struct usbd_interface intf0;
static struct usbd_interface intf1;

void usb_cdc_begin(void)
{
    usbd_desc_register(0, &cdc_descriptor);
    usbd_add_interface(0, usbd_cdc_acm_init_intf(0, &intf0));
    usbd_add_interface(0, usbd_cdc_acm_init_intf(0, &intf1));
    usbd_add_endpoint(0, &out_ep);
    usbd_add_endpoint(0, &in_ep);
    usbd_initialize(0, 0, event_handler);
}

size_t usb_cdc_write(const char *data, size_t len)
{
    if (!configured || tx_busy) {
        return 0;
    }
    if (len > sizeof(tx_buf)) {
        len = sizeof(tx_buf);
    }
    memcpy(tx_buf, data, len);
    tx_busy = true;
    usbd_ep_start_write(0, CDC_IN_EP, tx_buf, len);
    return len;
}
