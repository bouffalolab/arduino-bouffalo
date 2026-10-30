#ifndef UNOR4_BL616CL_CDC_LINE_CODING_CONTROL_H
#define UNOR4_BL616CL_CDC_LINE_CODING_CONTROL_H

#include <stdint.h>

namespace bridge {

struct CdcLineCodingHooks {
    bool (*disconnect_wifi)(void *context);
    bool (*enter_program_mode)(void *context);
    bool (*reset_with_boot_low)(void *context);
    bool (*update_user_uart_baud)(void *context, uint32_t baud);
    bool (*clear_user_uart_rx)(void *context);
    void *context;
};

/*
 * Applies the user-visible CDC baud-rate contract from the ESP32-S3 bridge.
 * USB callbacks submit the request; the main loop calls apply() so USB setup
 * handling never waits through the RA4M1 reset pulse sequence.
 */
class CdcLineCodingControl {
public:
    explicit CdcLineCodingControl(
        const CdcLineCodingHooks &hooks,
        uint32_t initial_baud = 115200U);

    bool apply(uint32_t baud);
    bool at_service_enabled() const;
    uint32_t baud() const;

private:
    CdcLineCodingHooks hooks_;
    uint32_t baud_;
};

} // namespace bridge

#endif
