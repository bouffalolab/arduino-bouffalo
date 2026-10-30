#include "HCIACLFlowControl.h"
#include <cassert>

int main() {
  HCIACLFlowControl flow;
  assert(flow.pending() == 0);
  assert(flow.queued(0) && flow.queued(0) && flow.queued(1));
  assert(flow.pending() == 3);
  flow.completed(0, 0);
  assert(flow.pending() == 3); // A zero completion must not release buffers.
  flow.completed(9, 10);
  assert(flow.pending() == 3); // Unknown handles cannot credit another link.
  flow.disconnected(0);
  assert(flow.pending() == 1); // Disconnect frees only that handle's packets.
  flow.completed(0, 2);
  assert(flow.pending() == 1); // A stale completion must not affect link 1.
  flow.completed(1, 100);
  assert(flow.pending() == 0); // Saturate per handle; never underflow.
  for (unsigned cycle = 0; cycle < 1000; ++cycle) {
    assert(flow.queued(0) && flow.queued(0));
    flow.disconnected(0); // Controller can omit completed events on disconnect.
    assert(flow.pending() == 0);
  }
  for (unsigned i = 0; i < 8; ++i) assert(flow.queued(i));
  assert(!flow.queued(9)); // Full handle table does not lose accounting.
  assert(flow.pending() == 8);
  flow.disconnected(3);
  assert(flow.queued(9));
  assert(flow.pending() == 8);
  flow.clear();
  assert(flow.pending() == 0);
  for (unsigned i = 0; i < 255; ++i) assert(flow.queued(1));
  assert(!flow.queued(1));
  assert(flow.pending() == 255);
  assert(!flow.queued(0xffff));
  flow.completed(1, 254);
  assert(flow.pending() == 1);
  flow.completed(1, 1);
  assert(flow.pending() == 0);
}
