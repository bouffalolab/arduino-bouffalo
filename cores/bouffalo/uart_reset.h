#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reset uart0 (index 0) or uart1 (index 1) to power-on register values through C code;
 * bl616cl_glb.h cannot compile as C++.
 */
void uart_reset(uint8_t index);

#ifdef __cplusplus
}
#endif
