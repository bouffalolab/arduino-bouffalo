#ifndef UNOR4_BL616CL_RA4_WATCHDOG_RECOVERY_H
#define UNOR4_BL616CL_RA4_WATCHDOG_RECOVERY_H

class HardwareSerial;

namespace bridge {

class Ra4BootstrapControl;

/*
 * Re-enter the existing RA4M1 application after a diagnostic BL616CL
 * watchdog reset without rewriting RA4M1 Flash. This is enabled only for the
 * watchdog diagnostic image.
 */
bool ra4_watchdog_recover_application(
    Ra4BootstrapControl &bootstrap,
    HardwareSerial &serial);

} // namespace bridge

#endif
