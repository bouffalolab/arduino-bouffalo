#ifndef UNOR4_BL616CL_RECOVERY_CONTROL_H
#define UNOR4_BL616CL_RECOVERY_CONTROL_H

#include <stdint.h>

namespace bridge {

/* Wrap-safe comparison for the deferred response-drain deadline. */
inline bool bl616cl_recovery_window_elapsed(uint32_t now, uint32_t ready_at)
{
    return static_cast<int32_t>(now - ready_at) >= 0;
}

/*
 * Queue a BL616CL self-update/recovery request from the USB HID callback.
 * The reset itself is performed later from the Arduino loop context.
 */
bool bl616cl_recovery_request();
bool bl616cl_reset_request();
bool bl616cl_recovery_poll();

} // namespace bridge

#endif
