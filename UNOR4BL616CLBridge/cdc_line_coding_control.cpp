#include "cdc_line_coding_control.h"

namespace bridge {

CdcLineCodingControl::CdcLineCodingControl(
    const CdcLineCodingHooks &hooks,
    uint32_t initial_baud)
    : hooks_(hooks), baud_(initial_baud)
{
}

bool CdcLineCodingControl::apply(uint32_t baud)
{
    if (baud == 0U) {
        return false;
    }

    baud_ = baud;
    bool succeeded = true;

    if (baud == 1200U) {
        if (hooks_.enter_program_mode != nullptr) {
            succeeded = hooks_.enter_program_mode(hooks_.context) && succeeded;
        } else {
            succeeded = false;
        }
    } else if (baud == 2400U) {
        if (hooks_.reset_with_boot_low != nullptr) {
            succeeded = hooks_.reset_with_boot_low(hooks_.context) && succeeded;
        } else {
            succeeded = false;
        }
    } else if (hooks_.update_user_uart_baud != nullptr) {
        succeeded = hooks_.update_user_uart_baud(hooks_.context, baud) && succeeded;
    } else {
        succeeded = false;
    }

    if (hooks_.clear_user_uart_rx != nullptr) {
        succeeded = hooks_.clear_user_uart_rx(hooks_.context) && succeeded;
    } else {
        succeeded = false;
    }

    /*
     * BOOT/RESET must not depend on the Wi-Fi/FHOST control path returning.
     * A pending STA/AP disconnect can wait for asynchronous WPA work; leave
     * radio cleanup best-effort and only after the RA4M1 has entered its
     * requested state and stale user-UART bytes have been discarded.
     */
    if ((baud == 1200U || baud == 2400U) &&
        hooks_.disconnect_wifi != nullptr) {
        succeeded = hooks_.disconnect_wifi(hooks_.context) && succeeded;
    }

    return succeeded;
}

bool CdcLineCodingControl::at_service_enabled() const
{
    return baud_ != 1200U && baud_ != 2400U;
}

uint32_t CdcLineCodingControl::baud() const
{
    return baud_;
}

} // namespace bridge
