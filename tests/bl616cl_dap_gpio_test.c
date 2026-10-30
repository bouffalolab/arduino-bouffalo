#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bflb_gpio.h"

/*
 * Include the production implementation so the fake GPIO event log shares
 * this translation unit with Free-DAP's static configuration helpers.
 */
#include "../UNOR4BL616CLBridge/freedap.c"

static void expect(int value, const char *message)
{
    if (!value) {
        fprintf(stderr, "test failure: %s\n", message);
        exit(1);
    }
}

static void expect_event(
    unsigned int index,
    enum bflb_gpio_fake_operation operation,
    uint8_t pin,
    uint32_t config,
    const char *message)
{
    const struct bflb_gpio_fake_event *event = bflb_gpio_fake_event(index);
    expect(event != 0, message);
    expect(event->operation == operation, message);
    expect(event->pin == pin, message);
    expect(event->config == config, message);
}

static int request(const uint8_t *request_data, int request_size, uint8_t *response)
{
    memset(response, 0, 64U);
    return dap_process_request((uint8_t *)request_data, request_size, response, 64);
}

int main(void)
{
    uint8_t response[64];
    const uint32_t output_config =
        GPIO_INPUT | GPIO_OUTPUT | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_1;

    bflb_gpio_fake_reset_events();
    dap_init();
    expect(bflb_gpio_fake_count() == 3U, "DAP setup releases three RA4 nets");
    expect_event(
        0U,
        BFLB_GPIO_FAKE_DEINIT,
        UNOR4_BL616CL_RA4_SWDIO_PIN,
        0U,
        "DAP setup releases SWDIO");
    expect_event(
        1U,
        BFLB_GPIO_FAKE_DEINIT,
        UNOR4_BL616CL_RA4_SWCLK_PIN,
        0U,
        "DAP setup releases SWCLK");
    expect_event(
        2U,
        BFLB_GPIO_FAKE_DEINIT,
        UNOR4_BL616CL_RA4_RESET_PIN,
        0U,
        "DAP setup releases nRESET");

    bflb_gpio_fake_reset_events();
    {
        const uint8_t request_data[] = { 0x02U, 0x01U };
        expect(
            request(request_data, sizeof(request_data), response) == 2,
            "SWD connect response size");
        expect(response[0] == 0x02U && response[1] == 1U, "SWD connect succeeds");
    }
    expect(bflb_gpio_fake_count() == 6U, "SWD connect configures three outputs");
    expect_event(
        0U,
        BFLB_GPIO_FAKE_SET,
        UNOR4_BL616CL_RA4_SWDIO_PIN,
        0U,
        "SWDIO latch is high before output enable");
    expect_event(
        1U,
        BFLB_GPIO_FAKE_INIT,
        UNOR4_BL616CL_RA4_SWDIO_PIN,
        output_config,
        "SWDIO is a floating output");
    expect_event(
        2U,
        BFLB_GPIO_FAKE_SET,
        UNOR4_BL616CL_RA4_SWCLK_PIN,
        0U,
        "SWCLK latch is high before output enable");
    expect_event(
        3U,
        BFLB_GPIO_FAKE_INIT,
        UNOR4_BL616CL_RA4_SWCLK_PIN,
        output_config,
        "SWCLK is a floating output");
    expect_event(
        4U,
        BFLB_GPIO_FAKE_SET,
        UNOR4_BL616CL_RA4_RESET_PIN,
        0U,
        "nRESET latch is high before output enable");
    expect_event(
        5U,
        BFLB_GPIO_FAKE_INIT,
        UNOR4_BL616CL_RA4_RESET_PIN,
        output_config,
        "nRESET is a floating output");

    bflb_gpio_fake_reset_events();
    {
        const uint8_t request_data[] = { 0x03U };
        expect(
            request(request_data, sizeof(request_data), response) == 2,
            "disconnect response size");
        expect(
            response[0] == 0x03U && response[1] == 0U,
            "disconnect succeeds");
    }
    expect(bflb_gpio_fake_count() == 3U, "disconnect releases three RA4 nets");
    expect_event(
        0U,
        BFLB_GPIO_FAKE_DEINIT,
        UNOR4_BL616CL_RA4_SWDIO_PIN,
        0U,
        "disconnect releases SWDIO");
    expect_event(
        1U,
        BFLB_GPIO_FAKE_DEINIT,
        UNOR4_BL616CL_RA4_SWCLK_PIN,
        0U,
        "disconnect releases SWCLK");
    expect_event(
        2U,
        BFLB_GPIO_FAKE_DEINIT,
        UNOR4_BL616CL_RA4_RESET_PIN,
        0U,
        "disconnect releases nRESET");

    return 0;
}
