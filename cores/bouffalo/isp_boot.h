#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 清除“复位后进入下载模式”的请求；Core 在 init() 中调用 */
void isp_boot_clear(void);

/* 请求复位后进入 ROM 下载模式（UART 与 USB），并立即上电复位；不返回 */
void isp_boot_reboot(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
