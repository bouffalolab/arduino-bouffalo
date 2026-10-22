#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void usb_cdc_begin(void);
/* 未枚举或上一包未发完时返回 0，不阻塞 */
size_t usb_cdc_write(const char *data, size_t len);

#ifdef __cplusplus
}
#endif
