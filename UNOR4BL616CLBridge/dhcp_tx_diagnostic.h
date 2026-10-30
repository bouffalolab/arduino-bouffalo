#pragma once
#include <stdint.h>

#ifndef BL616CL_DHCP_TX_DIAGNOSTIC
#define BL616CL_DHCP_TX_DIAGNOSTIC 0
#endif

#if BL616CL_DHCP_TX_DIAGNOSTIC
#include <atomic>
#endif

namespace bridge {
#if BL616CL_DHCP_TX_DIAGNOSTIC
struct DhcpTxDiagnostic {
    std::atomic<uint32_t> released[3] = {};
    std::atomic<uint32_t> discarded[3] = {};
    std::atomic<uint32_t> acknowledged[3] = {};
};
inline DhcpTxDiagnostic &dhcp_tx_diagnostic()
{
    static DhcpTxDiagnostic counters;
    return counters;
}
inline void dhcp_tx_diagnostic_note(uint8_t type, uint32_t acknowledged, uint32_t discarded)
{
    const unsigned index = type == 2U ? 0U : type == 5U ? 1U : 2U;
    if (type != 2U && type != 5U && type != 6U) return;
    auto &counters = dhcp_tx_diagnostic();
    ++counters.released[index];
    if (discarded != 0U) ++counters.discarded[index];
    if (acknowledged != 0U) ++counters.acknowledged[index];
}
#endif

// Page52 exports counts only. Broadcast completion is not host receipt;
// each counter is atomic, but the complete snapshot is not a transaction.
inline void dhcp_tx_diagnostic_report(uint8_t *report)
{
    report[5] = 52U;
    report[6] = BL616CL_DHCP_TX_DIAGNOSTIC ? 1U : 0U;
    report[7] = 1U;
#if BL616CL_DHCP_TX_DIAGNOSTIC
    auto &counters = dhcp_tx_diagnostic();
    for (unsigned i = 0; i < 3U; ++i) {
        const uint32_t values[] = {counters.released[i].load(),
                                  counters.discarded[i].load(),
                                  counters.acknowledged[i].load()};
        for (unsigned j = 0; j < 3U; ++j)
            for (unsigned byte = 0; byte < 4U; ++byte)
                report[8U + 12U*i + 4U*j + byte] = values[j] >> (8U*byte);
    }
#endif
}
}
