#include "../UNOR4BL616CLBridge/ra4_bootstrap_control.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

struct FakeIo {
    std::string events;
    size_t failure_event = static_cast<size_t>(-1);
    size_t event_count = 0U;
    int8_t boot_level = 1;
    int8_t reset_level = 1;
};

[[noreturn]] void fail(const char *message)
{
    std::cerr << "test failure: " << message << '\n';
    std::exit(1);
}

void expect(bool value, const char *message)
{
    if (!value) {
        fail(message);
    }
}

bool should_fail(FakeIo *io)
{
    const bool failed = io->event_count == io->failure_event;
    ++io->event_count;
    return failed;
}

bool input_float(void *context, uint8_t pin)
{
    FakeIo *io = static_cast<FakeIo *>(context);
    io->events += "I" + std::to_string(pin) + ";";
    return !should_fail(io);
}

bool output(void *context, uint8_t pin, bool high)
{
    FakeIo *io = static_cast<FakeIo *>(context);
    io->events += "O" + std::to_string(pin) + (high ? "H;" : "L;");
    if (pin == 14U) {
        io->boot_level = high ? 1 : 0;
    } else if (pin == 3U) {
        io->reset_level = high ? 1 : 0;
    }
    return !should_fail(io);
}

int8_t read_level(void *context, uint8_t pin)
{
    FakeIo *io = static_cast<FakeIo *>(context);
    return pin == 14U ? io->boot_level : io->reset_level;
}

bool delay_ms(void *context, uint32_t milliseconds)
{
    FakeIo *io = static_cast<FakeIo *>(context);
    io->events += "D" + std::to_string(milliseconds) + ";";
    return !should_fail(io);
}

bridge::Ra4BootstrapHooks hooks(FakeIo &io)
{
    return {
        input_float,
        output,
        read_level,
        delay_ms,
        &io,
    };
}

void expect_events(const FakeIo &io, const char *expected, const char *message)
{
    expect(io.events == expected, message);
}

} // namespace

int main()
{
    constexpr uint8_t kBoot = 14U;
    constexpr uint8_t kReset = 3U;

    FakeIo io;
    bridge::Ra4BootstrapControl control(kBoot, kReset, hooks(io));

    expect(control.begin(), "startup release succeeds");
    expect_events(io, "I3;I14;", "startup releases reset then boot");

    io = {};
    expect(control.begin_normal_boot(), "paired carrier normal startup succeeds");
    expect_events(io, "O14H;O3H;", "normal startup sets MD before releasing reset without a pulse");
    expect(control.diagnostic().operation == bridge::Ra4BootstrapOperation::kNormalBoot,
           "normal startup diagnostic operation");
    expect(control.diagnostic().succeeded && control.diagnostic().records[1].boot_level == 1 &&
           control.diagnostic().records[1].reset_level == 1, "normal startup pad levels");

    io = {};
    io.failure_event = 1U;
    expect(!control.begin_normal_boot(), "normal startup failure propagates");
    expect_events(io, "O14H;O3H;I3;I14;", "normal startup failure releases both nets");

    io = {};
    expect(control.enter_program_mode(), "program mode entry succeeds");
    expect_events(
        io,
        "O14H;O3L;D100;O3H;D100;O3L;D100;O3H;",
        "program mode entry preserves ESP32 bridge timing");
    const bridge::Ra4BootstrapDiagnostic &program_diagnostic = control.diagnostic();
    expect(
        program_diagnostic.operation ==
            bridge::Ra4BootstrapOperation::kEnterProgramMode,
        "program diagnostic operation");
    expect(!program_diagnostic.active, "program diagnostic completed");
    expect(program_diagnostic.succeeded, "program diagnostic success");
    expect(program_diagnostic.record_count == 8U, "program diagnostic step count");
    expect(
        program_diagnostic.records[0].boot_state ==
            static_cast<uint8_t>(bridge::Ra4BootstrapPinState::kHigh) &&
            program_diagnostic.records[0].reset_state ==
            static_cast<uint8_t>(bridge::Ra4BootstrapPinState::kReleased),
        "program diagnostic first state");
    expect(
        program_diagnostic.records[7].boot_level == 1 &&
            program_diagnostic.records[7].reset_level == 1,
        "program diagnostic final pad levels");

    io = {};
    expect(control.reset_with_boot_low(), "boot-low reset succeeds");
    expect_events(
        io,
        "O14L;O3H;D100;O3L;D100;O3H;",
        "boot-low reset preserves ESP32 bridge timing");

    io = {};
    expect(control.pulse_reset(), "reset pulse succeeds");
    expect_events(io, "O3L;D100;O3H;", "BOSSA final reset timing");

    io = {};
    expect(control.reset_to_application(), "application reset succeeds");
    expect_events(io, "O3L;D100;O3H;", "application reset preserves BOOT");

    io = {};
    io.failure_event = 2U;
    expect(!control.enter_program_mode(), "failed control operation reports failure");
    expect_events(
        io,
        "O14H;O3L;D100;I3;I14;",
        "failed control operation releases both control nets");
    const bridge::Ra4BootstrapDiagnostic &failed_diagnostic = control.diagnostic();
    expect(
        failed_diagnostic.operation ==
            bridge::Ra4BootstrapOperation::kEnterProgramMode,
        "failure preserves original operation");
    expect(!failed_diagnostic.active, "failure diagnostic completed");
    expect(!failed_diagnostic.succeeded, "failure diagnostic reports failure");
    expect(
        failed_diagnostic.record_count == 5U &&
            failed_diagnostic.records[3].step == 0xF1U &&
            failed_diagnostic.records[4].step == 0xF2U,
        "failure diagnostic records fail-safe release");

    return 0;
}
