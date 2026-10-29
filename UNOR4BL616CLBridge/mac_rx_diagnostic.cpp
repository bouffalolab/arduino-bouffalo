#include "mac_rx_diagnostic.h"
#include "udp_mac_fragment_observer.h"
#if BL616CL_MAC_RX_DIAGNOSTIC
extern "C" {
#include "sdk/mm/mm.h"
}
namespace {
// Whole-radio lifetime counters, including management/AP traffic. No flow claim.
// 0 entry, 1 HW failure, 2 exit, 3 descriptor busy, 4 prepare, 5 transfer fail,
// 6 reorder check, 7 queued, 8 removed, 9 timer, 10 release (timeout/free),
// 11 FHOST message, 12 forward, 13 delete, 14 net_al_input, 15 exit !upload,
// 16 data entry, 17 PN reject, 18 duplicate reject, 19 data retry,
// 20 data KEYIDV clear, 21 data HW failure, 22 buffer get, 23 buffer return,
// 24 empty buffer check, 25 failed get, 26 failed return.
// Events 15,19..21,25..26 are derived here and cannot be passed by callers.
uint32_t counts[27];
// CPU-only diagnostic words may live in WRAM. Preserve the OCRAM heap
// floor after upstream HardwareSerial added baud-rate bookkeeping.
#if defined(__riscv)
__attribute__((section(".wifi_ram.bridge_mac_last")))
#endif
uint32_t last_event, last_values[3];
#if defined(__riscv)
__attribute__((section(".wifi_ram.bridge_udp_mac")))
#endif
bridge::UdpMacFragmentObserver udp_mac;
}
extern "C" void bridge_udp_mac_fragment_observe(const void *llc, unsigned size, unsigned sn) {
    const uintptr_t flags = mm_lock_save();
    udp_mac.observe(static_cast<const uint8_t *>(llc), size, sn);
    mm_unlock_restore(flags);
}
extern "C" void fhost_bridge_rx_event(unsigned event, unsigned a, unsigned b, unsigned c) {
    if (event >= 25 || event == 15 || event == 19 || event == 20 || event == 21) return;
    const uintptr_t flags = mm_lock_save();
    ++counts[event];
    if (event == 2 && !a) ++counts[15];
    if (event == 16) {
        if (a) ++counts[19]; // data retry bit
        if (!b) ++counts[20]; // data HW key-id-valid bit clear
        if (!c) ++counts[21]; // data HW success bit clear
    }
    if (event == 22 && a) ++counts[25]; // failed queue read
    if (event == 23 && a) ++counts[26]; // failed queue write
    last_event = event;
    last_values[0] = a; last_values[1] = b; last_values[2] = c;
    mm_unlock_restore(flags);
}
#endif
namespace bridge {
void mac_rx_diagnostic_report(uint8_t page, uint8_t *report) {
    report[5] = page; report[6] = BL616CL_MAC_RX_DIAGNOSTIC ? 1 : 0;
    report[7] = 2; // MAC RX diagnostic layout version
#if BL616CL_MAC_RX_DIAGNOSTIC
    const auto put = [report](unsigned offset, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) report[offset+i] = value >> (8*i);
    };
    const uintptr_t flags = mm_lock_save();
    if (page == 48) {
        for (unsigned i = 0; i < 14; ++i) put(8+4*i, counts[i]);
    } else if (page == 49) {
        for (unsigned i = 14; i < 22; ++i) put(8+4*(i-14), counts[i]);
        put(40, last_event);
        for (unsigned i = 0; i < 3; ++i) put(44+4*i, last_values[i]);
    } else if (page == 50) {
        for (unsigned i = 22; i < 27; ++i) put(8+4*(i-22), counts[i]);
    } else if (page == 82) {
        report[7] = 1;
        put(8, udp_mac.total); put(12, udp_mac.id); put(16, udp_mac.count);
        put(20, udp_mac.mask); put(24, udp_mac.sequence);
        for (unsigned i = 0; i < 12; ++i) {
            report[28+2*i] = udp_mac.offsets[i];
            report[29+2*i] = udp_mac.offsets[i] >> 8;
        }
    }
    mm_unlock_restore(flags);
#endif
}
}
