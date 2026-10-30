#include "board_gpio_overlay.h"

#include "bflb_gpio.h"

void board_uartx_gpio_init(void)
{
    struct bflb_device_s *gpio = bflb_device_get_by_name("gpio");

    if (gpio == NULL) {
        return;
    }

    /*
     * RA4M1 AT UART. The DK BSP uses GPIO24/25, which are not connected to
     * the candidate UNO R4 carrier's AT UART.
     */
    bflb_gpio_uart_init(gpio, GPIO_PIN_6, GPIO_UART_FUNC_UART1_TX);
    bflb_gpio_uart_init(gpio, GPIO_PIN_7, GPIO_UART_FUNC_UART1_RX);
}

void board_usb_gpio_init(void)
{
    struct bflb_device_s *gpio = bflb_device_get_by_name("gpio");

    if (gpio == NULL) {
        return;
    }

    /*
     * Candidate carrier contract: GPIO33 is USB D-, GPIO32 is USB D+.
     * Both must remain analog USB pads; USB polarity needs hardware proof.
     */
    bflb_gpio_init(gpio, GPIO_PIN_32, GPIO_ANALOG | GPIO_SMT_EN | GPIO_DRV_0);
    bflb_gpio_init(gpio, GPIO_PIN_33, GPIO_ANALOG | GPIO_SMT_EN | GPIO_DRV_0);
}
