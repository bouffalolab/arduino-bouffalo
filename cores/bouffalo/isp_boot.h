#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Clear the retained ROM-download request during Core init().
 */
void isp_boot_clear(void);

/**
 * @brief Request ROM download mode over UART/USB and immediately perform a power-on reset.
 * Does not return.
 */
void isp_boot_reboot(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
