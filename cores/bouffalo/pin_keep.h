#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Release low-power pin retention through C code because bl616cl_pm.h indirectly
 * includes core_rv32.h, which cannot compile as C++.
 */
void pin_release_keep(uint8_t pin);

#ifdef __cplusplus
}
#endif
