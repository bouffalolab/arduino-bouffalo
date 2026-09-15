#include "board_overlay.h"

#include "bflb_acomp.h"
#include "bflb_clock.h"
#include "bflb_efuse.h"
#include "bflb_flash.h"
#include "bflb_gpio.h"
#include "bflb_irq.h"
#include "bflb_sec_mutex.h"
#include "bflb_sf_ctrl.h"
#include "bflb_xip_sflash.h"
#include "bflb_wo.h"
#include "bl616cl_glb.h"
#include "bl616cl_pm.h"
#include "bl616cl_tzc_sec.h"
#include "mm.h"

extern uint32_t __HeapBase;
extern uint32_t __HeapLimit;
extern uint32_t _heap_wifi_start;
extern uint32_t _heap_wifi_size;

extern void bflb_wo_set_console(struct bflb_device_s *dev);

static void system_bod_init(void)
{
    HBN_BOD_CFG_Type config = {
        .enableBod = 1,
        .enableBodInt = 1,
        .bodThreshold = 2,
        .enablePorInBod = 0,
    };

    HBN_Set_BOD_Cfg(&config);
}

static void system_bod_isr(int irq, void *arg)
{
    (void)irq;
    (void)arg;

    while (1) {
    }
}

static void ATTR_CLOCK_SECTION __attribute__((noinline)) system_clock_init(void)
{
    GLB_Power_On_XTAL_And_PLL_CLK(GLB_XTAL_40M, GLB_PLL_WIFIPLL);
    GLB_Set_MCU_System_CLK(GLB_MCU_SYS_CLK_TOP_WIFIPLL_320M);
    HBN_Set_MCU_XCLK_Sel(HBN_MCU_XCLK_XTAL);
    CPU_Set_MTimer_CLK(ENABLE, BL_MTIMER_SOURCE_CLOCK_MCU_XCLK,
                       Clock_System_Clock_Get(BL_SYSTEM_CLOCK_XCLK) / 1000000 - 1);

#ifdef CONFIG_WIFI6
    GLB_PER_Clock_UnGate(GLB_AHB_CLOCK_IP_WIFI_PHY |
                         GLB_AHB_CLOCK_IP_WIFI_MAC_PHY |
                         GLB_AHB_CLOCK_IP_WIFI_PLATFORM);
    GLB_AHB_MCU_Software_Reset(GLB_AHB_MCU_SW_WIFI);
#endif
}

static void peripheral_clock_init(void)
{
    PERIPHERAL_CLOCK_ADC_DAC_ENABLE();
    PERIPHERAL_CLOCK_SEC_ENABLE();
    PERIPHERAL_CLOCK_DMA0_ENABLE();
    PERIPHERAL_CLOCK_UART0_ENABLE();
    PERIPHERAL_CLOCK_UART1_ENABLE();
    PERIPHERAL_CLOCK_SPI0_ENABLE();
    PERIPHERAL_CLOCK_I2C0_ENABLE();
    PERIPHERAL_CLOCK_PWM0_ENABLE();
    PERIPHERAL_CLOCK_TIMER0_1_WDG_ENABLE();
    PERIPHERAL_CLOCK_IR_ENABLE();
    PERIPHERAL_CLOCK_I2S_ENABLE();
    PERIPHERAL_CLOCK_USB_ENABLE();
    PERIPHERAL_CLOCK_CAN_ENABLE();
    PERIPHERAL_CLOCK_AUDIO_ENABLE();
    PERIPHERAL_CLOCK_CKS_ENABLE();
    PERIPHERAL_CLOCK_PEC_ENABLE();

    GLB_Set_UART_CLK(ENABLE, HBN_UART_CLK_XCLK, 0);
    GLB_Set_SPI0_CLK(ENABLE, GLB_SPI_CLK_MCU_MUXPLL_160M, 0);
    GLB_Set_SPI1_CLK(ENABLE, GLB_SPI_CLK_MCU_MUXPLL_160M, 0);
    GLB_Set_DBI_CLK(ENABLE, GLB_DBI_CLK_MCU_MUXPLL_160M, 0);
    GLB_Set_I2C_CLK(ENABLE, GLB_I2C_CLK_XCLK, 0);
    GLB_Set_ADC_CLK(ENABLE, GLB_ADC_CLK_XCLK, 19);
    GLB_Set_DIG_CLK_Sel(GLB_DIG_CLK_XCLK);
    GLB_Set_DIG_512K_CLK(ENABLE, ENABLE, 0x4E);
    GLB_Set_PWM1_IO_Sel(GLB_PWM1_IO_SINGLE_END);
    GLB_Set_IR_CLK(ENABLE, GLB_IR_CLK_SRC_XCLK, 19);
    GLB_Set_CAM_CLK(ENABLE, GLB_CAM_CLK_WIFIPLL_96M, 3);
    GLB_Set_PEC_CLK(ENABLE, GLB_PEC_CLK_MCU_MUXPLL_160M, 0);
    GLB_Set_PKA_CLK_Sel(GLB_PKA_CLK_WIFIPLL_320M);
    GLB_Swap_MCU_SPI_0_MOSI_With_MISO(0);
}

#ifdef CONFIG_HIGH_ISR_STACK
static void bflb_wfa_init(void)
{
    extern void interrupt1_handler(void);
    extern void csi_vic_set_prio(int32_t IRQn, uint32_t priority);

    bflb_irq_attach(MAC_INT_PROT_TRIGGER_IRQn,
                    (irq_callback)interrupt1_handler, NULL);
    bflb_irq_enable(MAC_INT_PROT_TRIGGER_IRQn);
    csi_vic_set_prio(MAC_INT_PROT_TRIGGER_IRQn, 2);
}
#endif

void ram_heap_init(void)
{
    size_t heap_len;

    mem_manager_init();

    heap_len = (size_t)&__HeapLimit - (size_t)&__HeapBase;
#ifdef CONFIG_MEM_HEAP5_EN
    mm_register_heap(MM_HEAP_OCRAM_0, "OCRAM", MM_ALLOCATOR_HEAP5,
                     &__HeapBase, heap_len);
    mm_register_heap(MM_HEAP_WRAM_0, "WRAM", MM_ALLOCATOR_HEAP5,
                     &_heap_wifi_start, (uintptr_t)&_heap_wifi_size);
#else
    mm_register_heap(MM_HEAP_OCRAM_0, "OCRAM", MM_ALLOCATOR_TLSF,
                     &__HeapBase, heap_len);
    mm_register_heap(MM_HEAP_WRAM_0, "WRAM", MM_ALLOCATOR_TLSF,
                     &_heap_wifi_start, (uintptr_t)&_heap_wifi_size);
#endif
}

static void ebreak_cpu(void)
{
    __ASM volatile("ebreak");
}

static void console_init(void)
{
#ifdef CONFIG_CONSOLE_WO
    struct bflb_device_s *wo = bflb_device_get_by_name("wo");
    if (wo != NULL) {
        /*
         * GPIO8 is the dedicated BL616CL log TX on the final carrier.
         * Keep UART0 free for the RA4M1 user bridge on GPIO34/35.
         */
        bflb_wo_uart_init(wo, CONFIG_CONSOLE_UART_BAUDRATE, GPIO_PIN_8);
        bflb_wo_set_console(wo);
    }
#endif
}

void board_init(void)
{
    int flash_result = -1;
    uintptr_t irq_state = bflb_irq_save();

#ifndef CONFIG_BOARD_FLASH_INIT_SKIP
    flash_result = bflb_flash_init();
#ifndef CONFIG_BOARD_FLASH_LOW_SPEED
#ifdef CONFIG_BOARD_FLASH_96M
    if (board_set_flash_hs(GLB_SFLASH_CLK_WIFIPLL_96M) != 1) {
        board_set_flash_hs(GLB_SFLASH_CLK_MUXPLL_80M);
    }
#else
    board_set_flash_hs(GLB_SFLASH_CLK_MUXPLL_80M);
#endif
#endif
#endif

    system_clock_init();
    peripheral_clock_init();

    bflb_irq_initialize();
#ifdef CONFIG_WIFI6
    extern void interrupt0_handler(void);
    bflb_irq_attach(WIFI_IRQn, (irq_callback)interrupt0_handler, NULL);
    bflb_irq_enable(WIFI_IRQn);
#endif
#ifdef CONFIG_HIGH_ISR_STACK
    bflb_wfa_init();
#endif

    /*
     * Console output is write-only on GPIO8. It must not use UART0 because
     * GPIO34/35 are the RA4M1 user UART.
     */
    console_init();

    bflb_irq_attach(BOD_IRQn, system_bod_isr, NULL);
    bflb_irq_enable(BOD_IRQn);
    system_bod_init();
    ram_heap_init();
    bflb_sec_mutex_init();
    bflb_irq_restore(irq_state);

    (void)flash_result;

    *(volatile uint32_t *)MCU_MISC_BASE |=
        0x1U << MCU_MISC_REG_MCU_INFRA_TIMEOUT_EN_POS;
    *(volatile uint32_t *)MCU_MISC_BASE &=
        ~(0x1U << MCU_MISC_DEC_ERR_RSP_POS);

    bflb_irq_attach(BMX_MCU_BUS_ERR_IRQn, (irq_callback)ebreak_cpu, NULL);
    bflb_irq_attach(BMX_MCU_TO_IRQn, (irq_callback)ebreak_cpu, NULL);
    bflb_irq_enable(BMX_MCU_BUS_ERR_IRQn);
    bflb_irq_enable(BMX_MCU_TO_IRQn);
}

void board_acomp_init(void)
{
    struct bflb_device_s *gpio = bflb_device_get_by_name("gpio");
    struct bflb_acomp_config_s config = {
        .mux_en = ENABLE,
        .pos_chan_sel = AON_ACOMP_CHAN_ADC3,
        .neg_chan_sel = AON_ACOMP_CHAN_VIO_X_SCALING_FACTOR_1,
        .vio_sel = DEFAULT_ACOMP_VREF_1V65,
        .scaling_factor = AON_ACOMP_SCALING_FACTOR_1,
        .bias_prog = AON_ACOMP_BIAS_POWER_MODE1,
        .hysteresis_pos_volt = AON_ACOMP_HYSTERESIS_VOLT_NONE,
        .hysteresis_neg_volt = AON_ACOMP_HYSTERESIS_VOLT_NONE,
    };

    if (gpio == NULL) {
        return;
    }

    bflb_gpio_init(gpio, GPIO_PIN_3, GPIO_ANALOG | GPIO_DRV_0);
    bflb_gpio_init(gpio, GPIO_PIN_20, GPIO_ANALOG | GPIO_DRV_0);
    bflb_acomp_init(AON_ACOMP0_ID, &config);
    config.pos_chan_sel = AON_ACOMP_CHAN_ADC0;
    bflb_acomp_init(AON_ACOMP1_ID, &config);
}
