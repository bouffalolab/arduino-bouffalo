#include "bl616cl_recovery_control.h"

#include <Arduino.h>
#include <stdint.h>

extern "C" {
int HBN_Set_User_Boot_Config(uint8_t ubCfg);
int bl_sys_reset_por(void);
}

namespace bridge {
namespace {

volatile bool recovery_requested = false;
volatile bool reset_requested = false;
volatile uint32_t request_ready_at = 0U;

constexpr uint32_t kResponseDrainWindowMs = 50U;

} // namespace

bool bl616cl_recovery_request()
{
    recovery_requested = true;
    request_ready_at = millis() + kResponseDrainWindowMs;
    return true;
}

bool bl616cl_reset_request()
{
    reset_requested = true;
    request_ready_at = millis() + kResponseDrainWindowMs;
    return true;
}

bool bl616cl_recovery_poll()
{
    if (!recovery_requested && !reset_requested) {
        return false;
    }

    /* Let the AT task drain the queued OK before resetting the chip. */
    if (!bl616cl_recovery_window_elapsed(millis(), request_ready_at)) {
        return false;
    }

    const bool bootloader = recovery_requested;
    recovery_requested = false;
    reset_requested = false;

    /*
     * BootROM uses selection 1 for interface/download and 2 for Flash boot.
     * This is the BL616CL SDK path used by boot2_isp and the SDK board shell;
     * it replaces ESP32's usb_persist_restart(RESTART_BOOTLOADER).
     */
    if (HBN_Set_User_Boot_Config(bootloader ? 1U : 2U) != 0) {
        return false;
    }

    (void)bl_sys_reset_por();
    return true;
}

} // namespace bridge
