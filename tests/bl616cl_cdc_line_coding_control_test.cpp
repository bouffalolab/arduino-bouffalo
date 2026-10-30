#include "../UNOR4BL616CLBridge/cdc_line_coding_control.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

struct FakeControl {
    std::string events;
    bool fail_disconnect = false;
    bool fail_program = false;
    bool fail_reset = false;
    bool fail_baud = false;
    bool fail_clear = false;
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

bool disconnect_wifi(void *context)
{
    FakeControl *control = static_cast<FakeControl *>(context);
    control->events += "D;";
    return !control->fail_disconnect;
}

bool enter_program_mode(void *context)
{
    FakeControl *control = static_cast<FakeControl *>(context);
    control->events += "P;";
    return !control->fail_program;
}

bool reset_with_boot_low(void *context)
{
    FakeControl *control = static_cast<FakeControl *>(context);
    control->events += "R;";
    return !control->fail_reset;
}

bool update_user_uart_baud(void *context, uint32_t baud)
{
    FakeControl *control = static_cast<FakeControl *>(context);
    control->events += "B" + std::to_string(baud) + ";";
    return !control->fail_baud;
}

bool clear_user_uart_rx(void *context)
{
    FakeControl *control = static_cast<FakeControl *>(context);
    control->events += "C;";
    return !control->fail_clear;
}

bridge::CdcLineCodingHooks hooks(FakeControl &control)
{
    return {
        disconnect_wifi,
        enter_program_mode,
        reset_with_boot_low,
        update_user_uart_baud,
        clear_user_uart_rx,
        &control,
    };
}

void expect_events(const FakeControl &control, const char *expected, const char *message)
{
    expect(control.events == expected, message);
}

} // namespace

int main()
{
    FakeControl fake;
    bridge::CdcLineCodingControl control(hooks(fake));

    expect(control.at_service_enabled(), "AT service starts enabled");
    expect(control.baud() == 115200U, "initial baud is 115200");

    expect(control.apply(9600U), "normal baud change succeeds");
    expect_events(fake, "B9600;C;", "normal baud updates UART and clears RX");
    expect(control.at_service_enabled(), "normal baud keeps AT service enabled");
    expect(control.baud() == 9600U, "normal baud becomes active");

    fake = {};
    expect(control.apply(1200U), "1200 baud program sequence succeeds");
    expect_events(
        fake,
        "P;C;D;",
        "1200 enters program mode before best-effort Wi-Fi disconnect");
    expect(!control.at_service_enabled(), "1200 baud pauses AT service");

    fake = {};
    expect(control.apply(2400U), "2400 baud reset sequence succeeds");
    expect_events(
        fake,
        "R;C;D;",
        "2400 resets with boot low before best-effort Wi-Fi disconnect");
    expect(!control.at_service_enabled(), "2400 baud pauses AT service");

    fake = {};
    expect(!control.apply(0U), "zero baud is ignored");
    expect_events(fake, "", "zero baud does not touch control hardware");
    expect(control.baud() == 2400U, "zero baud does not change active mode");

    fake = {};
    fake.fail_disconnect = true;
    expect(!control.apply(1200U), "disconnect failure is reported");
    expect_events(
        fake,
        "P;C;D;",
        "program sequence and RX clear complete before Wi-Fi disconnect failure");

    return 0;
}
