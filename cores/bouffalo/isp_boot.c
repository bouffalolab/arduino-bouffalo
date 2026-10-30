/*
 * Enter ROM download mode like the SDK reboot uart command: set the user boot request and perform
 * a power-on reset. HBN_RSV2 retains the request across software resets. The uploader warm reset
 * leaves it set, so firmware startup clears it to prevent later software resets from reentering
 * ROM. The sequence follows bl_sys_reset_por(), omitting reset-reason recording because its libsys
 * object pulls in low-power parameters and the entire Wi-Fi link group. Keep this code in C:
 * core_rv32.h, included by bl616cl_hbn.h, cannot compile as C++.
 */
#include "bl616cl_glb.h"
#include "bl616cl_hbn.h"

#include "isp_boot.h"

/**
 * @brief Clear the retained software request to enter ROM after reset.
 */
void isp_boot_clear(void)
{
    if (HBN_Get_User_Boot_Config() != 0) {
        HBN_Set_User_Boot_Config(0);
    }
}

/**
 * @brief Set the ROM-download request and perform the nonreturning power-on reset sequence.
 */
void isp_boot_reboot(void)
{
    __disable_irq();
    HBN_Set_User_Boot_Config(1);

    HBN_32K_Sel(0); /* Switch the 32 kHz clock back to RC32K. */
    HBN_Power_Off_Xtal_32K();
    GLB_AHB_MCU_Software_Reset(GLB_AHB_MCU_SW_WIFI);
    GLB_AHB_MCU_Software_Reset(GLB_AHB_MCU_SW_BTDM);
    GLB_AHB_MCU_Software_Reset(GLB_AHB_MCU_SW_BLE2);
    arch_delay_ms(10);
    GLB_SW_POR_Reset();
    for (;;) {
    }
}

/**
 * @brief Public interface declared in Arduino.h.
 */
void resetToBootloader(void)
{
    isp_boot_reboot();
}
