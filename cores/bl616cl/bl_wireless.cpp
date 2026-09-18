// Platform wireless bring-up for BL616CL, called from init() before the
// scheduler starts -- the same place the SDK examples do it in main()
// (examples/wifi/macsw_bare/main.c calls rfparam_init() there):
//
//   rfparam_init()       load the RF calibration parameters (flash/eFuse)
//                        into the PHY.  WiFi and BLE share one RF chain, so
//                        this runs exactly once, before either stack.
//   bl_sys_em_config()   program the GLB EM/WRAM hardware split from the
//                        linked __LD_CONFIG_EM_SEL value.  The BT controller
//                        only does this itself in beacon-only builds
//                        (CONFIG_BLE_BEACON_ONLY), so the combined BT+BLE
//                        flavors (uarthci/m2s1) need the platform to do it.
//
// The PHY RF itself is brought up by each stack's own path (wl_init/phy_init
// for WiFi, btble_rf_init for the BLE controller); the phyrf rf_init() is not
// called by any SDK example on the BL616 family, so it is deliberately left
// alone here.
//
// The protocol stacks themselves (wl80211/wifi_mgmr, btble_controller) stay
// lazy: they need task context and are started by their respective users.
// This function is idempotent.

#include <Arduino.h>

#include "bflb_uart.h"

extern "C" {
int32_t rfparam_init(uint32_t base_addr, void *rf_para, uint32_t apply_flag);
const char *wl_get_version(void);
int bl_sys_em_config(void);
}

/* Same debug channel the BLE port uses: raw UART0 writes (GPIO34/35 at
 * 2 Mbaud) survive the USB init that rebinds the printf console later. */
static void wireless_dbg(const char *msg)
{
    struct bflb_device_s *uart = bflb_device_get_by_name("uart0");
    if (uart == NULL) {
        return;
    }
    while (*msg != '\0') {
        bflb_uart_putchar(uart, *msg++);
    }
}

extern "C" void bl_wireless_init(void)
{
    static bool ready = false;
    if (ready) {
        return;
    }
    ready = true;

    int32_t ret = rfparam_init(0, NULL, 0);
    bl_sys_em_config();

    /* Forensics: wl_cfg lives at WL_API_RMEM_ADDR (0x20010600), status(32b)
     * @+0, mode(8b) @+4 (1=wlan 2=bz 3=dual); GLB_SRAM_CFG3[3:0] is the
     * EM_SEL split bl_sys_em_config() just programmed. */
    volatile uint8_t *cfg = (volatile uint8_t *)0x20010600UL;
    volatile uint32_t *cfg3 = (volatile uint32_t *)0x2000060CUL;
    char buf[160];
    snprintf(buf, sizeof(buf),
             "[rfparam] ret=%ld wl=%s cfg_status=%08lx mode=%u em_sel=%lu\r\n",
             (long)ret, wl_get_version(),
             (unsigned long)*(volatile uint32_t *)cfg,
             (unsigned)cfg[4],
             (unsigned long)(*cfg3 & 0xFUL));
    wireless_dbg(buf);
}

/* lwIP's random hook: this platform's lwipopts map LWIP_RAND() to bl_rand(),
 * and igmp.c (always pulled in because mbedtls is linked with
 * --whole-archive) references it, so every sketch needs a definition.
 * Weak: esp32-compat's esp32_wifi.cpp provides its own for Bridge builds. */
extern "C" __attribute__((weak)) int bl_rand(void)
{
    static uint32_t seed = 0x12345678;
    seed = seed * 1103515245U + 12345U;
    return (int)(seed >> 16);
}
