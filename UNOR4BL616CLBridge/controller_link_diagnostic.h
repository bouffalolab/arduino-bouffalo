#pragma once
#include <stdint.h>

#ifndef BL616CL_CONTROLLER_LINK_DIAGNOSTIC
#define BL616CL_CONTROLLER_LINK_DIAGNOSTIC 0
#endif

namespace bridge {

// Numeric observations only. No peer addresses, packet bytes or RF register dumps.
// The caller serializes note/report against controller interrupts.
class ControllerLinkDiagnostic {
public:
    void note(uint32_t ms, uint32_t event, uint32_t link, uint32_t role,
              uint32_t interval, uint32_t events, uint32_t flags,
              uint32_t reason, uint32_t clock);
    void report(uint8_t page, uint8_t *report) const;
private:
    uint32_t sequence_ = 0;
    uint32_t entries_[16][10] = {};
};

void controller_link_diagnostic_report(uint8_t page, uint8_t *report);
}
