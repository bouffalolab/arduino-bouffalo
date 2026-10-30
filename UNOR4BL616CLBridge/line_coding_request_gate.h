#ifndef UNOR4_BL616CL_LINE_CODING_REQUEST_GATE_H
#define UNOR4_BL616CL_LINE_CODING_REQUEST_GATE_H

#include <stdint.h>

namespace bridge {

// Only baud-rate transitions control the RA4M1 bootstrap or user UART.
bool should_queue_line_coding_action(uint32_t previous_baud, uint32_t requested_baud);

} // namespace bridge

#endif
