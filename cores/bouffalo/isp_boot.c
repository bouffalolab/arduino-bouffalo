/*
 * Enter ROM download mode like the SDK reboot uart command: set the user boot request and perform
 * a power-on reset. HBN_RSV2 retains the request across software resets. The uploader warm reset
 * leaves it set, so firmware startup clears it to prevent later software resets from reentering
 * ROM. The sequence follows bl_sys_reset_por(), omitting reset-reason recording because its libsys
 * object pulls in low-power parameters and the entire Wi-Fi link group. For the same reason the
 * link recipe wraps bl_sys_reset_por() so that SDK callers such as the OTA engine get the version
 * below: with SDK v2.3.39 the Wi-Fi objects carry SYSINIT records, so linking libsys's copy would
 * start Wi-Fi in every OTA sketch. A wrap rather than a second definition keeps sketches that use
 * other functions of that libsys object linkable. Keep this code in C: core_rv32.h, included by
 * bl616cl_hbn.h, cannot compile as C++.
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
 * @brief Reset the radios and perform a power-on reset with interrupts disabled; does not return.
 */
static void __attribute__((noreturn)) por_reset(void)
{
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
 * @brief Set the ROM-download request and perform the nonreturning power-on reset sequence.
 */
void isp_boot_reboot(void)
{
    __disable_irq();
    HBN_Set_User_Boot_Config(1);
    por_reset();
}

/**
 * @brief SDK power-on reset used by components such as the OTA engine; does not return. Callers of
 * bl_sys_reset_por() reach it through the link recipe's --wrap (see the file comment).
 */
int __wrap_bl_sys_reset_por(void)
{
    __disable_irq();
    por_reset();
}

/**
 * @brief Public interface declared in Arduino.h.
 */
void resetToBootloader(void)
{
    isp_boot_reboot();
}
