#include "tcp_receive_window.h"
#if BL616CL_TCP_RECEIVE_WINDOW_BYTES
// Fixed across boot. lwIP owns window accounting, ACKs, queued data and PCB
// teardown. This limits each flow, not aggregate memory across all 24 slots.
extern "C" unsigned bridge_tcp_receive_window(void) {
    return BL616CL_TCP_RECEIVE_WINDOW_BYTES;
}
#endif
