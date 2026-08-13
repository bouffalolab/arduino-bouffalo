#ifndef BL616CL_ESP32_COMPAT_TINYUSB_H_
#define BL616CL_ESP32_COMPAT_TINYUSB_H_

#include <stdint.h>

typedef int esp_event_base_t;

typedef struct {
    uint32_t bit_rate;
} cdc_line_coding_t;

typedef struct {
    cdc_line_coding_t line_coding;
} arduino_usb_cdc_event_data_t;

enum {
    ARDUINO_USB_CDC_EVENTS = 1,
    ARDUINO_USB_CDC_LINE_CODING_EVENT = 0x11
};

enum {
    RESTART_BOOTLOADER = 0,
    RESTART_BOOTLOADER_OTA = 1
};

#ifdef __cplusplus
extern "C" {
#endif

void usb_persist_restart(int mode);

#ifdef __cplusplus
}
#endif

#endif
