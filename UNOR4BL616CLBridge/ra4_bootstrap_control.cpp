#include "ra4_bootstrap_control.h"

#include "Arduino.h"
#include "bflb_gpio.h"

namespace bridge {
namespace {

struct bflb_device_s *gpio_device()
{
    static struct bflb_device_s *device = nullptr;
    if (device == nullptr) {
        device = bflb_device_get_by_name("gpio");
    }
    return device;
}

bool bouffalo_configure_input_float(void *, uint8_t pin)
{
    struct bflb_device_s *gpio = gpio_device();
    if (gpio == nullptr) {
        return false;
    }

    bflb_gpio_deinit(gpio, pin);
    return true;
}

bool bouffalo_configure_output(void *, uint8_t pin, bool high)
{
    struct bflb_device_s *gpio = gpio_device();
    if (gpio == nullptr) {
        return false;
    }

    /*
     * Set the output latch before enabling output mode so a requested HIGH
     * cannot transiently drive the RA4M1 control net LOW.
     */
    if (high) {
        bflb_gpio_set(gpio, pin);
    } else {
        bflb_gpio_reset(gpio, pin);
    }
    /*
     * Keep the output driver enabled while also enabling the input buffer.
     * The latter makes bflb_gpio_read() report the pad level for bootstrap
     * diagnostics without changing the BOOT/RESET drive sequence.
     */
    bflb_gpio_init(
        gpio,
        pin,
        GPIO_OUTPUT | GPIO_INPUT | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_1);
    return true;
}

int8_t bouffalo_read_level(void *, uint8_t pin)
{
    struct bflb_device_s *gpio = gpio_device();
    if (gpio == nullptr) {
        return -1;
    }
    return bflb_gpio_read(gpio, pin) ? 1 : 0;
}

bool bouffalo_delay_ms(void *, uint32_t milliseconds)
{
    delay(milliseconds);
    return true;
}

} // namespace

Ra4BootstrapControl::Ra4BootstrapControl(
    uint8_t boot_pin,
    uint8_t reset_pin,
    const Ra4BootstrapHooks &hooks)
    : boot_pin_(boot_pin), reset_pin_(reset_pin), hooks_(hooks)
{
}

bool Ra4BootstrapControl::begin()
{
    return release(Ra4BootstrapOperation::kRelease);
}

bool Ra4BootstrapControl::begin_normal_boot()
{
    begin_operation(Ra4BootstrapOperation::kNormalBoot);
    if (!drive(boot_pin_, true, 1U) || !drive(reset_pin_, true, 2U)) {
        return fail_safe_release();
    }
    diagnostic_.active = false;
    diagnostic_.succeeded = true;
    return true;
}

bool Ra4BootstrapControl::release()
{
    return release(Ra4BootstrapOperation::kRelease);
}

bool Ra4BootstrapControl::release(Ra4BootstrapOperation operation)
{
    begin_operation(operation);
    bool released = true;
    if (hooks_.configure_input_float == nullptr) {
        record(1U, false);
        return false;
    }

    released = hooks_.configure_input_float(hooks_.context, reset_pin_) && released;
    if (released) {
        reset_state_ = Ra4BootstrapPinState::kReleased;
    }
    record(1U, released);
    const bool boot_released =
        hooks_.configure_input_float(hooks_.context, boot_pin_);
    released = boot_released && released;
    if (boot_released) {
        boot_state_ = Ra4BootstrapPinState::kReleased;
    }
    record(2U, released);
    diagnostic_.active = false;
    diagnostic_.succeeded = released;
    return released;
}

bool Ra4BootstrapControl::enter_program_mode()
{
    begin_operation(Ra4BootstrapOperation::kEnterProgramMode);
    if (!drive(boot_pin_, true, 1U) ||
        !drive(reset_pin_, false, 2U) ||
        !delay(kResetPulseMs, 3U) ||
        !drive(reset_pin_, true, 4U) ||
        !delay(kResetPulseMs, 5U) ||
        !drive(reset_pin_, false, 6U) ||
        !delay(kResetPulseMs, 7U) ||
        !drive(reset_pin_, true, 8U)) {
        return fail_safe_release();
    }
    diagnostic_.active = false;
    diagnostic_.succeeded = true;
    return true;
}

bool Ra4BootstrapControl::reset_with_boot_low()
{
    begin_operation(Ra4BootstrapOperation::kResetWithBootLow);
    if (!drive(boot_pin_, false, 1U) ||
        !drive(reset_pin_, true, 2U) ||
        !delay(kResetPulseMs, 3U) ||
        !drive(reset_pin_, false, 4U) ||
        !delay(kResetPulseMs, 5U) ||
        !drive(reset_pin_, true, 6U)) {
        return fail_safe_release();
    }
    diagnostic_.active = false;
    diagnostic_.succeeded = true;
    return true;
}

bool Ra4BootstrapControl::pulse_reset()
{
    begin_operation(Ra4BootstrapOperation::kResetToApplication);
    if (!drive(reset_pin_, false, 1U) ||
        !delay(kResetPulseMs, 2U) ||
        !drive(reset_pin_, true, 3U)) {
        return fail_safe_release();
    }
    diagnostic_.active = false;
    diagnostic_.succeeded = true;
    return true;
}

bool Ra4BootstrapControl::reset_to_application()
{
    return pulse_reset();
}

const Ra4BootstrapDiagnostic &Ra4BootstrapControl::diagnostic() const
{
    return diagnostic_;
}

bool Ra4BootstrapControl::drive(uint8_t pin, bool high, uint8_t step)
{
    const bool succeeded = hooks_.configure_output != nullptr &&
        hooks_.configure_output(hooks_.context, pin, high);
    if (pin == boot_pin_) {
        boot_state_ = high ? Ra4BootstrapPinState::kHigh : Ra4BootstrapPinState::kLow;
    } else if (pin == reset_pin_) {
        reset_state_ = high ? Ra4BootstrapPinState::kHigh : Ra4BootstrapPinState::kLow;
    }
    record(step, succeeded);
    return succeeded;
}

bool Ra4BootstrapControl::delay(uint32_t milliseconds, uint8_t step)
{
    const bool succeeded =
        hooks_.delay_ms != nullptr && hooks_.delay_ms(hooks_.context, milliseconds);
    record(step, succeeded);
    return succeeded;
}

void Ra4BootstrapControl::begin_operation(Ra4BootstrapOperation operation)
{
    ++diagnostic_.sequence;
    diagnostic_.operation = operation;
    diagnostic_.active = true;
    diagnostic_.succeeded = false;
    diagnostic_.record_count = 0U;
}

void Ra4BootstrapControl::record(uint8_t step, bool succeeded)
{
    if (diagnostic_.record_count >= Ra4BootstrapDiagnostic::kRecordCapacity) {
        return;
    }

    Ra4BootstrapDiagnosticRecord &entry =
        diagnostic_.records[diagnostic_.record_count++];
    entry.step = step;
    entry.boot_state = static_cast<uint8_t>(boot_state_);
    entry.reset_state = static_cast<uint8_t>(reset_state_);
    entry.boot_level = read_level(boot_pin_);
    entry.reset_level = read_level(reset_pin_);
    entry.succeeded = succeeded ? 1U : 0U;
}

int8_t Ra4BootstrapControl::read_level(uint8_t pin) const
{
    return hooks_.read_level != nullptr
        ? hooks_.read_level(hooks_.context, pin)
        : -1;
}

bool Ra4BootstrapControl::fail_safe_release()
{
    const bool was_active = diagnostic_.active;
    diagnostic_.active = false;
    diagnostic_.succeeded = false;
    if (hooks_.configure_input_float != nullptr) {
        const bool reset_released =
            hooks_.configure_input_float(hooks_.context, reset_pin_);
        if (reset_released) {
            reset_state_ = Ra4BootstrapPinState::kReleased;
        }
        record(0xF1U, reset_released);

        const bool boot_released =
            hooks_.configure_input_float(hooks_.context, boot_pin_);
        if (boot_released) {
            boot_state_ = Ra4BootstrapPinState::kReleased;
        }
        record(0xF2U, boot_released);
    } else if (!was_active) {
        begin_operation(Ra4BootstrapOperation::kFailSafeRelease);
        record(0xF0U, false);
        diagnostic_.active = false;
    }
    return false;
}

Ra4BootstrapHooks make_bouffalo_ra4_bootstrap_hooks()
{
    return {
        bouffalo_configure_input_float,
        bouffalo_configure_output,
        bouffalo_read_level,
        bouffalo_delay_ms,
        nullptr,
    };
}

} // namespace bridge
