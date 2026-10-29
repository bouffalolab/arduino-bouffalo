#include "line_coding_request_gate.h"

namespace bridge {

bool should_queue_line_coding_action(uint32_t previous_baud, uint32_t requested_baud)
{
    return requested_baud != 0U && requested_baud != previous_baud;
}

} // namespace bridge
