#ifndef UNOR4_BL616CL_RA4_BOOTSTRAP_CONTROL_H
#define UNOR4_BL616CL_RA4_BOOTSTRAP_CONTROL_H

#include <stdint.h>

namespace bridge {

enum class Ra4BootstrapOperation : uint8_t {
    kIdle = 0U,
    kRelease = 1U,
    kEnterProgramMode = 2U,
    kResetWithBootLow = 3U,
    kResetToApplication = 4U,
    kFailSafeRelease = 5U,
    kNormalBoot = 6U,
};

enum class Ra4BootstrapPinState : uint8_t {
    kUnknown = 0U,
    kHigh = 1U,
    kLow = 2U,
    kReleased = 3U,
};

struct Ra4BootstrapDiagnosticRecord {
    uint8_t step;
    uint8_t boot_state;
    uint8_t reset_state;
    int8_t boot_level;
    int8_t reset_level;
    uint8_t succeeded;
};

struct Ra4BootstrapDiagnostic {
    static constexpr uint8_t kRecordCapacity = 8U;

    uint32_t sequence;
    Ra4BootstrapOperation operation;
    bool active;
    bool succeeded;
    uint8_t record_count;
    Ra4BootstrapDiagnosticRecord records[kRecordCapacity];
};

struct Ra4BootstrapHooks {
    bool (*configure_input_float)(void *context, uint8_t pin);
    bool (*configure_output)(void *context, uint8_t pin, bool high);
    int8_t (*read_level)(void *context, uint8_t pin);
    bool (*delay_ms)(void *context, uint32_t milliseconds);
    void *context;
};

class Ra4BootstrapControl {
public:
    static constexpr uint32_t kResetPulseMs = 100U;

    Ra4BootstrapControl(uint8_t boot_pin, uint8_t reset_pin, const Ra4BootstrapHooks &hooks);

    /*
     * Leave both RA4M1 control nets high impedance. This is the required
     * startup state until the final carrier pull network is hardware-verified.
     */
    bool begin();
    bool release();

    /* Verified paired carrier: match ESP32-S3 startup, MD HIGH before RESET
     * HIGH. Do not pulse RESET or enter the double-tap SAM-BA sequence. */
    bool begin_normal_boot();

    /*
     * Preserve the existing ESP32-S3 bridge timing used by BOSSA and the USB
     * CDC 1200-baud control request. The original electrical boot polarity is
     * retained here without inferring a new BL616CL carrier polarity.
     */
    bool enter_program_mode();

    /*
     * Preserve the existing ESP32-S3 bridge USB CDC 2400-baud sequence. On
     * UNO R4 WiFi this selects RA4M1 ROM DFU for bootloader recovery, not a
     * user-Sketch runtime reset. It is unrelated to the BL616CL ISP strap.
     */
    bool reset_with_boot_low();

    /* Equivalent to the final reset step after RA4M1 programming. */
    bool pulse_reset();

    const Ra4BootstrapDiagnostic &diagnostic() const;

    /*
     * Preserve the current BOOT level and pulse RESET. This matches the
     * original ESP32-S3 BOSSA OTA implementation. SAM-BA's K# command is
     * still the primary application handoff; this method is only an optional
     * explicit hardware reset for diagnostics.
     */
    bool reset_to_application();

private:
    bool release(Ra4BootstrapOperation operation);
    bool drive(uint8_t pin, bool high, uint8_t step);
    bool delay(uint32_t milliseconds, uint8_t step);
    void begin_operation(Ra4BootstrapOperation operation);
    void record(uint8_t step, bool succeeded);
    int8_t read_level(uint8_t pin) const;
    bool fail_safe_release();

    uint8_t boot_pin_;
    uint8_t reset_pin_;
    Ra4BootstrapHooks hooks_;
    Ra4BootstrapPinState boot_state_ = Ra4BootstrapPinState::kUnknown;
    Ra4BootstrapPinState reset_state_ = Ra4BootstrapPinState::kUnknown;
    Ra4BootstrapDiagnostic diagnostic_ = {};
};

Ra4BootstrapHooks make_bouffalo_ra4_bootstrap_hooks();

} // namespace bridge

#endif
