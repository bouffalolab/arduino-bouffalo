#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 软件复位 UART 控制器（0 为 uart0，1 为 uart1），寄存器回到上电值。bl616cl_glb.h 不能按 C++ 编译，故经 C 文件调用 */
void uart_reset(uint8_t index);

#ifdef __cplusplus
}
#endif
