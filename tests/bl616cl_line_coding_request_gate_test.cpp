#include "../UNOR4BL616CLBridge/line_coding_request_gate.h"

#include <cstdlib>
#include <iostream>

namespace {

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

} // namespace

int main()
{
    expect(
        !bridge::should_queue_line_coding_action(115200U, 115200U),
        "duplicate normal baud must not queue another action");
    expect(
        !bridge::should_queue_line_coding_action(1200U, 1200U),
        "duplicate program baud must not repeat bootstrap");
    expect(
        !bridge::should_queue_line_coding_action(2400U, 2400U),
        "duplicate reset baud must not repeat reset");
    expect(
        !bridge::should_queue_line_coding_action(115200U, 0U),
        "zero baud must not queue a control action");
    expect(
        bridge::should_queue_line_coding_action(115200U, 1200U),
        "1200 transition must queue bootstrap");
    expect(
        bridge::should_queue_line_coding_action(1200U, 2400U),
        "2400 transition must queue reset");
    expect(
        bridge::should_queue_line_coding_action(2400U, 115200U),
        "normal baud transition must queue UART update");

    return 0;
}
