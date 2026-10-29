#ifndef UNOR4_BL616CL_WIFI_STA_DISCONNECT_DIAGNOSTIC_H
#define UNOR4_BL616CL_WIFI_STA_DISCONNECT_DIAGNOSTIC_H

#include <stdint.h>

namespace bridge {
struct WifiStaDisconnectDiagnostic {
    uint32_t attempts;
    uint32_t events;
    uint8_t stage;
    int8_t result;
    uint8_t next;
    uint8_t count;
    uint8_t stages[16];
    int8_t results[16];
};
WifiStaDisconnectDiagnostic wifi_sta_disconnect_diagnostic_snapshot();
}

extern "C" void fhost_bridge_sta_disconnect_stage(uint8_t stage, int result);
#endif
