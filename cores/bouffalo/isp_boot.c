/*
 * 软件进入 ROM 下载模式，与 SDK 的 reboot uart 命令相同：设置用户启动配置后上电复位。
 * 该配置保存在 HBN 域（HBN_RSV2），软件复位后仍保留，ROM 看到它就进入下载模式。
 * 烧录工具最后用 warm reset 启动新固件时配置仍为 1，所以固件启动时要清除，
 * 否则之后的软件复位会再次进入下载模式。
 * 复位序列取自 SDK 的 bl_sys_reset_por()，只去掉记录复位原因一步：那一步所在的 libsys
 * 对象引用低功耗参数区，会把 Wi-Fi 协议栈整组链入不用 Wi-Fi 的 sketch。
 * bl616cl_hbn.h 间接包含的 core_rv32.h 不能按 C++ 编译，故放在 C 文件中。
 */
#include "bl616cl_glb.h"
#include "bl616cl_hbn.h"

#include "isp_boot.h"

void isp_boot_clear(void)
{
    if (HBN_Get_User_Boot_Config() != 0) {
        HBN_Set_User_Boot_Config(0);
    }
}

void isp_boot_reboot(void)
{
    __disable_irq();
    HBN_Set_User_Boot_Config(1);

    HBN_32K_Sel(0); /* 32 kHz 时钟切回 RC32K */
    HBN_Power_Off_Xtal_32K();
    GLB_AHB_MCU_Software_Reset(GLB_AHB_MCU_SW_WIFI);
    GLB_AHB_MCU_Software_Reset(GLB_AHB_MCU_SW_BTDM);
    GLB_AHB_MCU_Software_Reset(GLB_AHB_MCU_SW_BLE2);
    arch_delay_ms(10);
    GLB_SW_POR_Reset();
    for (;;) {
    }
}
