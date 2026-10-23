#pragma once

/*
 * USB CDC ACM 设备（Serial 的底层）。板卡在 boards.txt 中给出 USB_VID、USB_PID、USB_MANUFACTURER、
 * USB_PRODUCT 时启用；描述符、收发缓冲、1200 bps touch 和线路变化回调都在 usb_cdc_port.c。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 主机设置的线路参数，取值同 CDC SET_LINE_CODING */
struct usb_cdc_port_line {
    uint32_t baud;
    uint8_t stop_bits; /* 0：1 位，1：1.5 位，2：2 位 */
    uint8_t parity;    /* 0 无，1 奇，2 偶，3 mark，4 space */
    uint8_t data_bits;
    bool dtr;
    bool rts;
};

/* 注册描述符并启动 USB 设备；调度器运行后调用一次 */
void usb_cdc_port_init(void);

/* 设备已配置且主机置 DTR（端口已打开） */
bool usb_cdc_port_connected(void);
void usb_cdc_port_get_line(struct usb_cdc_port_line *line);

/* 是否响应 1200 bps touch，默认是；关闭时取消已安排、尚未执行的 touch */
void usb_cdc_port_set_touch_reset(bool enable);
/*
 * 每个 SET_LINE_CODING、SET_CONTROL_LINE_STATE 请求之后，在线路任务中调用一次 callback；NULL 取消。
 * 注册之前的请求不补调。首次注册时创建线路任务，失败返回 false。
 */
bool usb_cdc_port_on_line_change(void (*callback)(void));

size_t usb_cdc_port_rx_count(void);
int usb_cdc_port_peek(void);
int usb_cdc_port_read(void);

/* 写入发送缓冲。端口未打开时丢弃并返回 0；缓冲满时等待，100 ms 无进展则返回已写入的字节数 */
size_t usb_cdc_port_write(const uint8_t *buf, size_t len);
size_t usb_cdc_port_tx_space(void);
/* 等待已写入的数据发出；端口未打开或 100 ms 无进展时返回 */
void usb_cdc_port_flush(void);

#ifdef __cplusplus
}
#endif
