// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2017-2022, Alex Taradov. All rights reserved.

#ifndef UNOR4_BL616CL_DAP_CONFIG_H
#define UNOR4_BL616CL_DAP_CONFIG_H

#include <stdint.h>

#include "bflb_gpio.h"

#include "ra4_pin_config.h"

#define DAP_CONFIG_DEFAULT_PORT DAP_PORT_SWD

/*
 * This is intentionally conservative until the manually wired carrier has
 * passed a real SWD attach gate and the delay loop has been calibrated.
 */
#define DAP_CONFIG_DEFAULT_CLOCK 100000
#define DAP_CONFIG_PACKET_SIZE 64
#define DAP_CONFIG_PACKET_COUNT 2
#define DAP_CONFIG_JTAG_DEV_COUNT 0
#define DAP_CONFIG_DELAY_CONSTANT 7700
#define DAP_CONFIG_FAST_CLOCK 2147483647

#define DAP_CONFIG_VENDOR_STR "Arduino"
#define DAP_CONFIG_PRODUCT_STR "UNO R4 WiFi CMSIS-DAP Adapter"
#define DAP_CONFIG_SER_NUM_STR "BL616CL-PROVISIONAL"
#define DAP_CONFIG_CMSIS_DAP_VER_STR "2.0.0"
#define DAP_CONFIG_FW_VER_STR "0.6.0"

#define DAP_CONFIG_PERFORMANCE_ATTR

static inline struct bflb_device_s *dap_config_gpio_device(void)
{
    static struct bflb_device_s *gpio;
    if (gpio == 0) {
        gpio = bflb_device_get_by_name("gpio");
    }
    return gpio;
}

static inline void dap_config_input_float(uint8_t pin)
{
    struct bflb_device_s *gpio = dap_config_gpio_device();
    if (gpio != 0) {
        bflb_gpio_deinit(gpio, pin);
    }
}

static inline void dap_config_output(uint8_t pin, int value)
{
    struct bflb_device_s *gpio = dap_config_gpio_device();
    if (gpio == 0) {
        return;
    }

    if (value) {
        bflb_gpio_set(gpio, pin);
    } else {
        bflb_gpio_reset(gpio, pin);
    }
    /*
     * BL616CL separates input-enable and output-enable. Keep input enabled
     * while driving so DAP_SWJ_PINS can report the actual pad level.
     */
    bflb_gpio_init(
        gpio,
        pin,
        GPIO_INPUT | GPIO_OUTPUT | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_1);
}

static inline void DAP_CONFIG_SWCLK_TCK_write(int value)
{
    struct bflb_device_s *gpio = dap_config_gpio_device();
    if (gpio != 0) {
        if (value) {
            bflb_gpio_set(gpio, UNOR4_BL616CL_RA4_SWCLK_PIN);
        } else {
            bflb_gpio_reset(gpio, UNOR4_BL616CL_RA4_SWCLK_PIN);
        }
    }
}

static inline void DAP_CONFIG_SWDIO_TMS_write(int value)
{
    struct bflb_device_s *gpio = dap_config_gpio_device();
    if (gpio != 0) {
        if (value) {
            bflb_gpio_set(gpio, UNOR4_BL616CL_RA4_SWDIO_PIN);
        } else {
            bflb_gpio_reset(gpio, UNOR4_BL616CL_RA4_SWDIO_PIN);
        }
    }
}

static inline void DAP_CONFIG_TDI_write(int value)
{
    (void)value;
}

static inline void DAP_CONFIG_TDO_write(int value)
{
    (void)value;
}

static inline void DAP_CONFIG_nTRST_write(int value)
{
    (void)value;
}

static inline void DAP_CONFIG_nRESET_write(int value)
{
    struct bflb_device_s *gpio = dap_config_gpio_device();
    if (gpio != 0) {
        if (value) {
            bflb_gpio_set(gpio, UNOR4_BL616CL_RA4_RESET_PIN);
        } else {
            bflb_gpio_reset(gpio, UNOR4_BL616CL_RA4_RESET_PIN);
        }
    }
}

static inline int DAP_CONFIG_SWCLK_TCK_read(void)
{
    struct bflb_device_s *gpio = dap_config_gpio_device();
    return gpio != 0 && bflb_gpio_read(gpio, UNOR4_BL616CL_RA4_SWCLK_PIN);
}

static inline int DAP_CONFIG_SWDIO_TMS_read(void)
{
    struct bflb_device_s *gpio = dap_config_gpio_device();
    return gpio != 0 && bflb_gpio_read(gpio, UNOR4_BL616CL_RA4_SWDIO_PIN);
}

static inline int DAP_CONFIG_TDO_read(void)
{
    return 0;
}

static inline int DAP_CONFIG_TDI_read(void)
{
    return 0;
}

static inline int DAP_CONFIG_nTRST_read(void)
{
    return 0;
}

static inline int DAP_CONFIG_nRESET_read(void)
{
    struct bflb_device_s *gpio = dap_config_gpio_device();
    return gpio != 0 && bflb_gpio_read(gpio, UNOR4_BL616CL_RA4_RESET_PIN);
}

static inline void DAP_CONFIG_SWCLK_TCK_set(void)
{
    DAP_CONFIG_SWCLK_TCK_write(1);
}

static inline void DAP_CONFIG_SWCLK_TCK_clr(void)
{
    DAP_CONFIG_SWCLK_TCK_write(0);
}

static inline void DAP_CONFIG_SWDIO_TMS_in(void)
{
    dap_config_input_float(UNOR4_BL616CL_RA4_SWDIO_PIN);
}

static inline void DAP_CONFIG_SWDIO_TMS_out(void)
{
    dap_config_output(UNOR4_BL616CL_RA4_SWDIO_PIN, 1);
}

static inline void DAP_CONFIG_SETUP(void)
{
    dap_config_input_float(UNOR4_BL616CL_RA4_SWDIO_PIN);
    dap_config_input_float(UNOR4_BL616CL_RA4_SWCLK_PIN);
    dap_config_input_float(UNOR4_BL616CL_RA4_RESET_PIN);
}

static inline void DAP_CONFIG_DISCONNECT(void)
{
    DAP_CONFIG_SETUP();
}

static inline void DAP_CONFIG_CONNECT_SWD(void)
{
    dap_config_output(UNOR4_BL616CL_RA4_SWDIO_PIN, 1);
    dap_config_output(UNOR4_BL616CL_RA4_SWCLK_PIN, 1);
    dap_config_output(UNOR4_BL616CL_RA4_RESET_PIN, 1);
}

static inline void DAP_CONFIG_CONNECT_JTAG(void)
{
}

static inline void DAP_CONFIG_LED(int index, int state)
{
    (void)index;
    (void)state;
}

static inline void DAP_CONFIG_DELAY(uint32_t cycles)
{
    volatile uint32_t count = cycles;
    while (count-- != 0U) {
        __asm__ volatile("nop");
    }
}

#endif
