#ifndef UNOR4_BL616CL_BOARD_OVERLAY_H
#define UNOR4_BL616CL_BOARD_OVERLAY_H

/*
 * Project-local bridge BSP overlay.
 *
 * The upstream bl616cldk board implementation detects this header and
 * suppresses its board.c. Keep the full carrier contract here rather than
 * changing the reference SDK checkout.
 */

#include "board_flash_psram.h"
#include "board_gpio_overlay.h"

void board_init(void);
void board_acomp_init(void);

#define APP_PWM_TRI_CH_U  (PWM_CH1)
#define APP_PWM_TRI_CH_V  (PWM_CH2)
#define APP_PWM_TRI_CH_W  (PWM_CH3)
#define APP_PWM_PIN_U_H   (GPIO_PIN_18)
#define APP_PWM_PIN_U_L   (GPIO_PIN_19)
#define APP_PWM_PIN_V_H   (GPIO_PIN_20)
#define APP_PWM_PIN_V_L   (GPIO_PIN_21)
#define APP_PWM_PIN_W_H   (GPIO_PIN_22)
#define APP_PWM_PIN_W_L   (GPIO_PIN_23)
#define APP_HALL_PIN_U    (GPIO_PIN_12)
#define APP_HALL_PIN_V    (GPIO_PIN_13)
#define APP_HALL_PIN_W    (GPIO_PIN_14)
#define APP_KEY_0         (GPIO_PIN_15)
#define APP_KEY_1         (GPIO_PIN_24)
#define APP_KEY_2         (GPIO_PIN_25)
#define APP_CURRENT_CH_U  (ADC_CHANNEL_2)
#define APP_CURRENT_CH_V  (ADC_CHANNEL_3)
#define APP_CURRENT_CH_W  (ADC_CHANNEL_4)
#define APP_CURRENT_PIN_U (GPIO_PIN_2)
#define APP_CURRENT_PIN_V (GPIO_PIN_3)
#define APP_CURRENT_PIN_W (GPIO_PIN_4)
#define APP_TEST_PIN      (GPIO_PIN_5)

#define DEFAULT_ACOMP_VREF_1V65 33

#define DEFAULT_TEST_UART                "uart1"
#define DEFAULT_TEST_UART_DMA_TX_REQUEST DMA_REQUEST_UART1_TX
#define DEFAULT_TEST_UART_DMA_RX_REQUEST DMA_REQUEST_UART1_RX
#define DEFAULT_TEST_UART_DMA_TDR        DMA_ADDR_UART1_TDR
#define DEFAULT_TEST_UART_DMA_RDR        DMA_ADDR_UART1_RDR

#endif
