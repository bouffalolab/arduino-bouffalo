#ifndef UNOR4_BL616CL_LINE_CODING_REQUEST_QUEUE_H
#define UNOR4_BL616CL_LINE_CODING_REQUEST_QUEUE_H

#include <stdint.h>

namespace bridge {

/*
 * CDC SET_LINE_CODING can arrive again while the main loop is carrying out
 * the 300 ms RA4M1 bootstrap pulse. Keep transitions ordered so a following
 * normal baud request cannot replace a pending 1200/2400 control request.
 *
 * The USB callback is the only producer and the Arduino loop is the only
 * consumer, so volatile byte indexes are sufficient for this fixed SPSC
 * queue on the target.
 */
class LineCodingRequestQueue {
public:
    static constexpr uint8_t kCapacity = 8U;

    bool push(uint32_t baud);
    bool pop(uint32_t *baud);
    bool empty() const;

private:
    volatile uint8_t head_ = 0U;
    volatile uint8_t tail_ = 0U;
    uint32_t baud_[kCapacity] = {};
};

} // namespace bridge

#endif
