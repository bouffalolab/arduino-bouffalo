#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 解除引脚的低功耗保持。bl616cl_pm.h 间接包含的 core_rv32.h 不能按 C++ 编译，故经 C 文件调用 */
void pin_release_keep(uint8_t pin);

#ifdef __cplusplus
}
#endif
