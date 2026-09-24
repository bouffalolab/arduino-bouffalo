// btblecontroller port implementation backed by the HCIVirtualTransport
// queues instead of a hardware UART.
//
// The controller library references btble_uart_read / btble_uart_write /
// btble_uart_flow_on / btble_uart_flow_off through its rwip_eif_api table
// (plf/refip/src/arch/main/arch_main.c), so defining them here wires the
// controller's HCI engine to the AT-facing virtual transport:
//
//   host HCI bytes (AT+HCI_WRITE) -> hci_h2c_push -> armed read -> controller
//   controller events/ACL -> btble_uart_write -> hci_c2h_push -> AT+HCI_READ
//
// The stock port drives these functions from a UART ISR; here the read
// callback is invoked from the task that delivered the data (see
// hci_port_deliver), which is a strictly safer context.

#include "ble/ble_hci_port.h"
#include "utility/hci_transport_internal.h"

#include <Arduino.h>
#include <FreeRTOS.h>

#include "bflb_uart.h"
#include "mm.h"

/* NOTE: rfparam_adapter.h/wl_api.h are intentionally NOT included here:
 * they declare wl_init() with C++ linkage, which conflicts with the
 * extern "C" declarations below (the phyrf lib exports plain C symbols). */

extern "C" void ensure_rfparam(void);
extern "C" void btble_controller_init(uint8_t task_priority);
extern "C" void btble_ke_event_schedule(void);
extern "C" void *wl_cfg_get(void *rmem);
extern "C" int8_t wl_init(void);
/* The uarthci flavor is built with -DCONFIG_WLAN_COEX, so rwip_init()
 * arms the BLE core's WLAN coexistence masks (txmsk/rxmsk=0x3): every TX
 * waits for a WLAN-arbiter grant that nobody drives on this board, and
 * the radio stays silent. m2s1 (which transmits fine) never enables it.
 * Disarm the coex interface right after init (see TODO.md phase 6). */
extern "C" void rwip_wlcoex_set(bool en);

/* BLE2 core register file (reg_blecore.h, ble2 build): direct MMIO probes
 * for bring-up forensics — version proves the core is clocked, INTSTAT/ET
 * show whether the scheduler ever programs an event. */
#define BLE2_RWBLECNTL_ADDR  0x28000800UL
#define BLE2_VERSION_ADDR    0x28000804UL
#define BLE2_INTCNTL0_ADDR   0x2800080CUL
#define BLE2_INTSTAT0_ADDR   0x28000810UL
#define BLE2_ACTFIFOSTAT_ADDR 0x28000824UL
#define BLE2_ETPTR_ADDR      0x2800082CUL
/* m2s1 flavor: the hci_tl command descriptors are initialized by the
 * host-side HCI driver (libblestack.a); without it the first direct
 * hci_tl_cmd_received() injection corrupts the heap. We init the driver
 * but never call bt_enable(), so the host stack itself stays dormant. */
extern "C" void hci_driver_init(void);

static bool s_started = false;

extern "C" uint32_t compat_get_free_heap(void)
{
    return (uint32_t)kfree_size(0);
}

/* BFLB_LOG=n stubs out printf in this build; write debug marks straight to
 * the board console UART0 (GPIO34/35 @ 2 Mbit/s, the FT232 on the bench). */
static void ble_dbg(const char *msg)
{
    struct bflb_device_s *uart = bflb_device_get_by_name("uart0");
    if (uart == NULL) {
        return;
    }
    while (*msg != '\0') {
        bflb_uart_putchar(uart, *msg++);
    }
}

/* Bring-up forensics: dump the full BLE core register block (0x28000800,
 * 16 words, same layout as the btble_cli `ble2dump` command) so the working
 * and silent states can be diffed register by register. */
void ble2_core_dump(const char *tag)
{
    static const char *nm[16] = {
        "cntl", "ver", "conf", "ic0", "is0", "ack0", "ic1", "is1",
        "ack1", "actfifo", "rxdesc", "etptr", "deepslcntl", "deepslwkup",
        "r15", "r16"
    };
    volatile uint32_t *r = (volatile uint32_t *)BLE2_RWBLECNTL_ADDR;
    char buf[560];
    int n = snprintf(buf, sizeof(buf), "[ble2/%s] ", tag);
    for (int i = 0; i < 16 && n < (int)sizeof(buf) - 24; i++) {
        n += snprintf(buf + n, sizeof(buf) - n, "%s=%08lx ",
                      nm[i], (unsigned long)r[i]);
        if ((i % 4) == 3 && n < (int)sizeof(buf) - 4) {
            n += snprintf(buf + n, sizeof(buf) - n, "\r\n");
        }
    }
    /* IP core block: timers / timestamp targets (the MAC scheduler heart). */
    volatile uint32_t *ip = (volatile uint32_t *)0x28000000UL;
    n += snprintf(buf + n, sizeof(buf) - n, "ip[%s] ", tag);
    static const struct { int idx; const char *name; } ipregs[] = {
        {0x00 / 4, "rwdmcntl"}, {0x0C / 4, "ic0"}, {0x10 / 4, "is0"},
        {0x18 / 4, "ic1"}, {0x1C / 4, "is1"},
        {0x38 / 4, "deepsllstat"}, {0x3C / 4, "enbpreset"},
        {0x60 / 4, "errtype"}, {0x64 / 4, "swprof"},
        {0xE0 / 4, "timgencntl"}, {0xE4 / 4, "finetimtgt"},
        {0xE8 / 4, "clkntgt1"}, {0xEC / 4, "hmicrotgt1"},
        {0x104 / 4, "finetimecnt"}, {0x110 / 4, "actschncntl"},
    };
    for (unsigned i = 0; i < sizeof(ipregs) / sizeof(ipregs[0]) &&
                        n < (int)sizeof(buf) - 24; i++) {
        n += snprintf(buf + n, sizeof(buf) - n, "%s=%08lx ",
                      ipregs[i].name,
                      (unsigned long)ip[ipregs[i].idx]);
    }
    n += snprintf(buf + n, sizeof(buf) - n, "\r\n");
    ble_dbg(buf);
}

/* Wide register dump for A/B diffing working vs silent controller state.
 * Mirrors the btble_cli `blewide` command: IP core 0x28000000..0x120 and
 * BLE core 0x28000800..0x880, four words per line. */
void ble_wide_dump(void)
{
    ble_dbg("WIDE-IP\r\n");
    for (uint32_t a = 0x28000000UL; a <= 0x28000120UL; a += 0x10) {
        volatile uint32_t *p = (volatile uint32_t *)a;
        char l[64];
        snprintf(l, sizeof(l), "%08lx: %08lx %08lx %08lx %08lx\r\n",
                 (unsigned long)a, (unsigned long)p[0], (unsigned long)p[1],
                 (unsigned long)p[2], (unsigned long)p[3]);
        ble_dbg(l);
    }
    ble_dbg("WIDE-BLE\r\n");
    for (uint32_t a = 0x28000800UL; a <= 0x28000880UL; a += 0x10) {
        volatile uint32_t *p = (volatile uint32_t *)a;
        char l[64];
        snprintf(l, sizeof(l), "%08lx: %08lx %08lx %08lx %08lx\r\n",
                 (unsigned long)a, (unsigned long)p[0], (unsigned long)p[1],
                 (unsigned long)p[2], (unsigned long)p[3]);
        ble_dbg(l);
    }
    ble_dbg("PHYDUMP\r\n");
    for (uint32_t a = 0x20000000UL; a <= 0x200001F0UL; a += 0x10) {
        volatile uint32_t *p = (volatile uint32_t *)a;
        char l[64];
        snprintf(l, sizeof(l), "%08lx: %08lx %08lx %08lx %08lx\r\n",
                 (unsigned long)a, (unsigned long)p[0], (unsigned long)p[1],
                 (unsigned long)p[2], (unsigned long)p[3]);
        ble_dbg(l);
    }
    for (uint32_t a = 0x20001000UL; a <= 0x200013F0UL; a += 0x10) {
        volatile uint32_t *p = (volatile uint32_t *)a;
        char l[64];
        snprintf(l, sizeof(l), "%08lx: %08lx %08lx %08lx %08lx\r\n",
                 (unsigned long)a, (unsigned long)p[0], (unsigned long)p[1],
                 (unsigned long)p[2], (unsigned long)p[3]);
        ble_dbg(l);
    }
    for (uint32_t a = 0x20002000UL; a <= 0x20002FF0UL; a += 0x10) {
        volatile uint32_t *p = (volatile uint32_t *)a;
        char l[64];
        snprintf(l, sizeof(l), "%08lx: %08lx %08lx %08lx %08lx\r\n",
                 (unsigned long)a, (unsigned long)p[0], (unsigned long)p[1],
                 (unsigned long)p[2], (unsigned long)p[3]);
        ble_dbg(l);
    }
    ble_dbg("PHYEND\r\n");
    ble_dbg("EMDUMP\r\n");
    {
        volatile uint32_t *em = (volatile uint32_t *)0x28010000UL;
        char l[80];
        for (int i = 0; i < 512; i += 8) {
            int n = snprintf(l, sizeof(l), "%05x:", i * 4);
            for (int j = 0; j < 8; j++) {
                n += snprintf(l + n, sizeof(l) - n, " %08lx",
                              (unsigned long)em[i + j]);
            }
            snprintf(l + n, sizeof(l) - n, "\r\n");
            ble_dbg(l);
        }
        ble_dbg("EMEND\r\n");
    }
    {
        volatile uint32_t *dfe = (volatile uint32_t *)0x20004640UL;
        volatile uint32_t *band = (volatile uint32_t *)0x20004264UL;
        char l[96];
        snprintf(l, sizeof(l), "DFE=%08lx BAND=%08lx\r\n",
                 (unsigned long)*dfe, (unsigned long)*band);
        ble_dbg(l);
    }
    {
        volatile uint32_t *coex = (volatile uint32_t *)0x28000950UL;
        char l[80];
        snprintf(l, sizeof(l),
                 "COEX0=%08lx COEX1=%08lx\r\n",
                 (unsigned long)coex[0], (unsigned long)coex[1]);
        ble_dbg(l);
    }
    ble_dbg("WIDE-END\r\n");
}

/* Trace sink for the instrumented controller lib (BT_BRINGUP_TRACE). */
extern "C" void btble_trace(const char *s)
{
    ble_dbg("[trc] ");
    ble_dbg(s);
}

/* WRAM/EM reachability diagnostics: GLB SRAM config, clock gates, and an
 * EM-window write/readback probe (MAC reads descriptors from EM, which is
 * carved out of WRAM via GLB_EM_SEL). */
void ble_wcheck_dump(void)
{
    char l[192];
    volatile uint32_t *glb = (volatile uint32_t *)0x20000000UL;
    snprintf(l, sizeof(l),
             "[wchk] SRAM_CFG0=%08lx CFG1=%08lx CFG2=%08lx CFG3=%08lx "
             "CGEN1=%08lx CGEN2=%08lx\r\n",
             (unsigned long)glb[0x600 / 4], (unsigned long)glb[0x604 / 4],
             (unsigned long)glb[0x608 / 4], (unsigned long)glb[0x60C / 4],
             (unsigned long)glb[0x580 / 4], (unsigned long)glb[0x584 / 4]);
    ble_dbg(l);

    /* EM window readback at several offsets (words at EM base + off). */
    volatile uint32_t *em = (volatile uint32_t *)0x28010000UL;
    static const uint32_t offs[] = {0x0000, 0x0010, 0x2000, 0x3F00, 0x4000, 0x7C00};
    for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        volatile uint32_t *p = (volatile uint32_t *)((uintptr_t)em + offs[i]);
        snprintf(l, sizeof(l), "[wchk] EM+%04lx: %08lx %08lx %08lx %08lx\r\n",
                 (unsigned long)offs[i], (unsigned long)p[0],
                 (unsigned long)p[1], (unsigned long)p[2], (unsigned long)p[3]);
        ble_dbg(l);
    }

    /* Write/readback probe at a high EM offset to prove CPU access. */
    {
        volatile uint32_t *p = (volatile uint32_t *)((uintptr_t)em + 0x7F00);
        uint32_t save = p[0];
        p[0] = 0xA5A5C001;
        uint32_t rb = p[0];
        p[0] = save;
        snprintf(l, sizeof(l), "[wchk] EM write/readback: wrote=a5a5c001 got=%08lx\r\n",
                 (unsigned long)rb);
        ble_dbg(l);
    }
}

/* Dump the full EM window (32K, 8 words/line) for payload-offset analysis. */
void ble_emfull_dump(void)
{
    volatile uint32_t *em = (volatile uint32_t *)0x28010000UL;
    ble_dbg("EMFULL\r\n");
    for (int i = 0; i < 8192; i += 8) {
        char l[96];
        int n = snprintf(l, sizeof(l), "%05x:", i * 4);
        for (int j = 0; j < 8; j++) {
            n += snprintf(l + n, sizeof(l) - n, " %08lx",
                          (unsigned long)em[i + j]);
        }
        snprintf(l + n, sizeof(l) - n, "\r\n");
        ble_dbg(l);
    }
    ble_dbg("EMENDALL\r\n");
}

/* Raw WRAM top-32K dump + EM-window mapping probe: verifies where the
 * CPU EM window (0x28010000) physically lands in WRAM, and whether the
 * BT MAC's view matches what the LLM wrote. */
void ble_rawmem_dump(void)
{
    char l[120];
    volatile uint32_t *em = (volatile uint32_t *)0x28010000UL;
    /* Mapping probe: write a magic through the EM window at three offsets,
     * then scan raw WRAM for it. */
    static const uint32_t probe_offs[] = {0x0, 0x1000, 0x4000};
    static const uint32_t magic[] = {0xE1A00001, 0xE1A00002, 0xE1A00003};
    for (int i = 0; i < 3; i++) {
        volatile uint32_t *pp = (volatile uint32_t *)((uintptr_t)em + probe_offs[i]);
        pp[0] = magic[i];
    }
    for (uint32_t a = 0x21018000UL; a < 0x21020000UL; a += 4) {
        uint32_t v = *(volatile uint32_t *)a;
        for (int i = 0; i < 3; i++) {
            if (v == magic[i]) {
                snprintf(l, sizeof(l),
                         "[wmap] EM+%lx mag%d found at raw %08lx (off %lx)\r\n",
                         (unsigned long)probe_offs[i], i,
                         (unsigned long)a,
                         (unsigned long)(a - 0x21018000UL));
                ble_dbg(l);
            }
        }
    }
    ble_dbg("RAWDUMP\r\n");
    for (uint32_t a = 0x2101C000UL; a < 0x21020000UL; a += 0x20) {
        volatile uint32_t *p32 = (volatile uint32_t *)a;
        int n = snprintf(l, sizeof(l), "%06lx:", (unsigned long)(a - 0x2101C000UL));
        for (int j = 0; j < 8; j++) {
            n += snprintf(l + n, sizeof(l) - n, " %08lx",
                          (unsigned long)p32[j]);
        }
        snprintf(l + n, sizeof(l) - n, "\r\n");
        ble_dbg(l);
    }
    ble_dbg("RAWEND\r\n");
}

bool ble_controller_start(void)
{
    if (s_started) {
        return true;
    }
    // Mark started before initialising so a re-entrant begin() (the AT
    // command retrying after a timeout) cannot start the controller twice.
    s_started = true;

    ble_dbg("[ble] rfparam start\r\n");
    ensure_rfparam();
    ble_dbg("[ble] em/rf init done\r\n");

    hci_transport_reset();
    ble_dbg("[ble] btble_controller_init ...\r\n");
    btble_controller_init((uint8_t)(configMAX_PRIORITIES - 1));
    ble_dbg("[ble] btble_controller_init done\r\n");
#if defined(USE_M2S1_CONTROLLER)
    hci_driver_init();
    ble_dbg("[ble] hci_driver_init done\r\n");
#endif
    return true;
}

extern "C" {

void btble_uart_pin_config(uint8_t uartid, uint8_t tx, uint8_t rx,
                           uint8_t cts, uint8_t rts)
{
    (void)uartid;
    (void)tx;
    (void)rx;
    (void)cts;
    (void)rts;
}

void btble_uart_init(uint8_t uartid)
{
    (void)uartid;
    hci_transport_reset();
}

int8_t btble_uart_reconfig(uint32_t baudrate, uint8_t flow_ctl_en,
                           uint8_t cts_pin, uint8_t rts_pin)
{
    (void)baudrate;
    (void)flow_ctl_en;
    (void)cts_pin;
    (void)rts_pin;
    return 0;
}

void btble_uart_flow_on(void)
{
}

bool btble_uart_flow_off(void)
{
    return true;
}

void btble_uart_finish_transfers(void)
{
}

/* The AT+BLECTR diagnostic reads the SRAM bring-up counters that the
 * 2026-09-18 instrumented controller build exported.  The stock 1.6.200+
 * controller library has no instrumentation, so provide a zeroed array to
 * keep the command linking (it simply reports no activity). */
extern "C" volatile uint32_t btble_bringup_ctr[16];
volatile uint32_t btble_bringup_ctr[16];

void btble_uart_read(uint8_t *bufptr, uint32_t size,
                     void (*callback)(void *, uint8_t), void *dummy)
{
    hci_port_arm_read(bufptr, size, callback, dummy);
    hci_port_deliver();
}

void btble_uart_write(const uint8_t *bufptr, uint32_t size,
                      void (*callback)(void *, uint8_t), void *dummy)
{
    hci_transport_note_write();
    hci_c2h_push(bufptr, size);
    if (callback != nullptr) {
        // The stock port signals completion from the UART ISR; calling the
        // callback inline from the controller task is equivalent here since
        // nothing asynchronous is involved.
        callback(dummy, 0);
        // The H4TL tx callback registers a deferred job (tx_done) which the
        // ROM wake chain never runs here; drain it synchronously so the HCI
        // TL tx state machine advances to the next queued event.
        btble_ke_event_schedule();
    }
}

}  // extern "C"
