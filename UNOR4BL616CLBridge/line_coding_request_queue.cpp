#include "line_coding_request_queue.h"

namespace bridge {

bool LineCodingRequestQueue::push(uint32_t baud)
{
    const uint8_t next = static_cast<uint8_t>((head_ + 1U) % kCapacity);
    if (next == tail_) {
        return false;
    }

    baud_[head_] = baud;
    head_ = next;
    return true;
}

bool LineCodingRequestQueue::pop(uint32_t *baud)
{
    if (baud == nullptr || tail_ == head_) {
        return false;
    }

    *baud = baud_[tail_];
    tail_ = static_cast<uint8_t>((tail_ + 1U) % kCapacity);
    return true;
}

bool LineCodingRequestQueue::empty() const
{
    return tail_ == head_;
}

} // namespace bridge
