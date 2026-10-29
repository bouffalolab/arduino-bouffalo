#include "../UNOR4BL616CLBridge/line_coding_request_queue.h"

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
    bridge::LineCodingRequestQueue queue;
    uint32_t baud = 0U;

    expect(queue.empty(), "new queue is empty");
    expect(queue.push(1200U), "enqueue bootstrap baud");
    expect(queue.push(230400U), "enqueue following normal baud");
    expect(!queue.empty(), "queue reports pending requests");
    expect(queue.pop(&baud) && baud == 1200U, "bootstrap remains first");
    expect(queue.pop(&baud) && baud == 230400U, "normal baud remains second");
    expect(queue.empty(), "queue is empty after ordered drain");
    expect(!queue.pop(&baud), "empty queue does not produce a request");

    for (uint8_t index = 0U;
         index < bridge::LineCodingRequestQueue::kCapacity - 1U;
         ++index) {
        expect(queue.push(9600U + index), "queue accepts every usable slot");
    }
    expect(!queue.push(57600U), "queue rejects overflow without overwriting");
    for (uint8_t index = 0U;
         index < bridge::LineCodingRequestQueue::kCapacity - 1U;
         ++index) {
        expect(queue.pop(&baud) && baud == 9600U + index, "queue preserves FIFO order");
    }
    expect(queue.empty(), "queue drains after full usable capacity");

    return 0;
}
