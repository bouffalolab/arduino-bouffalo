// Crash diagnosis for the BL616CL bring-up: override the SOC's exception
// handler, dump mcause/mepc/mtval straight into the CDC IN endpoint (the USB
// controller transmits from its FIFO autonomously, so the host can still
// receive this after the CPU halts), and stash a copy in no-init RAM.
#include <Arduino.h>
#include <stdint.h>

#include "usb_dc.h"

#define CRASH_MAGIC 0x43524153 /* "CRAS" */
#define CDC_IN_EP 0x83

static uint32_t g_crash_info[4]
    __attribute__((section(".nocache_noinit_ram"), used));

static void hex32(char *dst, uint32_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        dst[i] = digits[v & 0xF];
        v >>= 4;
    }
}

extern "C" void exception_entry(uintptr_t *regs)
{
    (void)regs;
    uint32_t cause = 0;
    uint32_t epc = 0;
    uint32_t tval = 0;
    __asm__ volatile("csrr %0, mcause" : "=r"(cause));
    __asm__ volatile("csrr %0, mepc" : "=r"(epc));
    __asm__ volatile("csrr %0, mtval" : "=r"(tval));

    g_crash_info[0] = CRASH_MAGIC;
    g_crash_info[1] = cause;
    g_crash_info[2] = epc;
    g_crash_info[3] = tval;

    char msg[64];
    int n = 0;
    msg[n++] = '\r';
    msg[n++] = '\n';
    const char *tag = "[CRASH] cause=";
    while (*tag != '\0' && n < (int)sizeof(msg) - 9) {
        msg[n++] = *tag++;
    }
    hex32(msg + n, cause);
    n += 8;
    tag = " epc=";
    while (*tag != '\0' && n < (int)sizeof(msg) - 9) {
        msg[n++] = *tag++;
    }
    hex32(msg + n, epc);
    n += 8;
    tag = " tval=";
    while (*tag != '\0' && n < (int)sizeof(msg) - 9) {
        msg[n++] = *tag++;
    }
    hex32(msg + n, tval);
    n += 8;
    msg[n++] = '\r';
    msg[n++] = '\n';

    usbd_ep_start_write(0, CDC_IN_EP, (const uint8_t *)msg, (uint32_t)n);

    for (;;) {
        __asm__ volatile("wfi");
    }
}

extern "C" void compat_get_crash_info(uint32_t *out)
{
    out[0] = g_crash_info[0];
    out[1] = g_crash_info[1];
    out[2] = g_crash_info[2];
    out[3] = g_crash_info[3];
}

extern "C" void compat_boot_marker(void)
{
    // Preserve a crash record captured by exception_entry().
    if (g_crash_info[0] == CRASH_MAGIC) {
        return;
    }
    if (g_crash_info[0] == 0x0000B007) {
        g_crash_info[1] += 1; /* cross-reset boot counter */
        return;
    }
    g_crash_info[0] = 0x0000B007;
    g_crash_info[1] = 1;
    g_crash_info[2] = 0x0000DEAD;
    g_crash_info[3] = 0x0000BEEF;
}
