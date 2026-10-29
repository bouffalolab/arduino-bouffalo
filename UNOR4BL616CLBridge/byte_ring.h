#ifndef UNOR4_BL616CL_BYTE_RING_H
#define UNOR4_BL616CL_BYTE_RING_H

#include <stddef.h>
#include <stdint.h>

namespace bridge {

template <size_t Capacity>
class ByteRing {
public:
    static_assert(Capacity > 1U, "ring capacity must reserve one empty slot");

    bool push(uint8_t value)
    {
        const size_t next = increment(head_);
        if (next == tail_) {
            return false;
        }

        data_[head_] = value;
        head_ = next;
        return true;
    }

    size_t read(uint8_t *destination, size_t limit)
    {
        if (destination == nullptr) {
            return 0U;
        }

        size_t count = 0U;
        while (count < limit && tail_ != head_) {
            destination[count++] = data_[tail_];
            tail_ = increment(tail_);
        }
        return count;
    }

    size_t write(const uint8_t *source, size_t size)
    {
        if (source == nullptr) {
            return 0U;
        }

        size_t count = 0U;
        while (count < size && push(source[count])) {
            ++count;
        }
        return count;
    }

    bool pop(uint8_t &value)
    {
        if (tail_ == head_) {
            return false;
        }

        value = data_[tail_];
        tail_ = increment(tail_);
        return true;
    }

    size_t size() const
    {
        const size_t head = head_;
        const size_t tail = tail_;
        return head >= tail ? head - tail : Capacity - tail + head;
    }

    size_t free() const
    {
        return Capacity - 1U - size();
    }

    bool peek(size_t offset, uint8_t &value) const
    {
        if (offset >= size()) {
            return false;
        }
        value = data_[(tail_ + offset) % Capacity];
        return true;
    }

    void truncate(size_t count)
    {
        if (count < size()) {
            head_ = (tail_ + count) % Capacity;
        }
    }

    void clear()
    {
        tail_ = head_;
    }

private:
    static size_t increment(size_t value)
    {
        ++value;
        return value == Capacity ? 0U : value;
    }

    uint8_t data_[Capacity] = {};
    volatile size_t head_ = 0U;
    volatile size_t tail_ = 0U;
};

} // namespace bridge

#endif
