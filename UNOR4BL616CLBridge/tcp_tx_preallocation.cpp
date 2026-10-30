#include "tcp_tx_preallocation.h"
static_assert(BL616CL_TCP_TX_EXTRA_BYTES >= -1 && BL616CL_TCP_TX_EXTRA_BYTES <= 1460,
    "Invalid TCP TX extra-byte diagnostic");
#if BL616CL_TCP_TX_EXTRA_BYTES >= 0
extern "C" unsigned bridge_tcp_tx_extra_bytes(void) {
    return BL616CL_TCP_TX_EXTRA_BYTES;
}
#endif
