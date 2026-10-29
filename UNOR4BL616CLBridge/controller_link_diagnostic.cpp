#include "controller_link_diagnostic.h"

#if BL616CL_CONTROLLER_LINK_DIAGNOSTIC
extern "C" {
#include "bflb_mtimer.h"
#include "sdk/mm/mm.h"
}
namespace {
// The linker/startup zeroes .wifi_ram with the WRAM BSS. Keep the optional
// 644-byte history out of the OCRAM heap budget; no retained state is needed.
__attribute__((section(".wifi_ram.bridge_controller_links")))
bridge::ControllerLinkDiagnostic observations;
__attribute__((section(".wifi_ram.bridge_controller_links")))
uint32_t advertising_rx[4] = {};
}

// 0=RX descriptor, 1=error-free CONNECT_IND, 2=parameters accepted,
// 3=RX status error. Count only; no address, PDU, or RF register is retained.
extern "C" void bridge_ble_adv_rx_observe(unsigned event)
{
    const uintptr_t saved = mm_lock_save();
    if (event < 4U) ++advertising_rx[event];
    mm_unlock_restore(saved);
}

extern "C" void bridge_ble_link_observe(unsigned event, unsigned clock,
    unsigned link, unsigned role, unsigned interval, unsigned events,
    unsigned flags, unsigned reason)
{
    const uint32_t ms = static_cast<uint32_t>(bflb_mtimer_get_time_ms());
    const uintptr_t saved = mm_lock_save();
    observations.note(ms, event, link, role, interval, events, flags, reason, clock);
    mm_unlock_restore(saved);
}
#endif

namespace bridge {
void ControllerLinkDiagnostic::note(uint32_t ms, uint32_t event, uint32_t link,
    uint32_t role, uint32_t interval, uint32_t events, uint32_t flags,
    uint32_t reason, uint32_t clock)
{
    uint32_t *row = entries_[sequence_ % 16U];
    row[0] = ++sequence_;
    row[1] = ms; row[2] = event; row[3] = link; row[4] = role;
    row[5] = interval; row[6] = events; row[7] = flags;
    row[8] = reason; row[9] = clock;
}

void ControllerLinkDiagnostic::report(uint8_t page, uint8_t *report) const
{
    uint32_t words[10] = {};
    if (page == 63U) {
        words[0] = sequence_;
        words[1] = 16U;
    } else if (page >= 64U && page <= 79U) {
        for (unsigned i = 0; i < 10; ++i) words[i] = entries_[page - 64U][i];
    }
    for (unsigned i = 0; i < 10; ++i)
        for (unsigned b = 0; b < 4; ++b)
            report[8U + 4U*i + b] = static_cast<uint8_t>(words[i] >> (8U*b));
}

void controller_link_diagnostic_report(uint8_t page, uint8_t *report)
{
    report[5] = page;
    report[6] = BL616CL_CONTROLLER_LINK_DIAGNOSTIC ? 1U : 0U;
    report[7] = 1U; // Additive page schema; existing feature pages retain their format.
#if BL616CL_CONTROLLER_LINK_DIAGNOSTIC
    const uintptr_t saved = mm_lock_save();
    if (page == 80U) {
        for (unsigned i = 0; i < 4U; ++i)
            for (unsigned b = 0; b < 4U; ++b)
                report[8U + 4U*i + b] = static_cast<uint8_t>(advertising_rx[i] >> (8U*b));
    } else {
        observations.report(page, report);
    }
    mm_unlock_restore(saved);
#endif
}
}
