#include "bl616cl_glb.h"

#include "uart_reset.h"

/**
 * @brief Restore the selected UART controller to its reset register state.
 */
void uart_reset(uint8_t index)
{
    GLB_AHB_MCU_Software_Reset(index == 0 ? GLB_AHB_MCU_SW_UART0 : GLB_AHB_MCU_SW_UART1);
}
