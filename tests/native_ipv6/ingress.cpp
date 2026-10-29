// Compile the production ingress unit against the same native lwIP test heap.
#include <cassert>
#include "sta_rx_ownership.cpp"
static bool locked;
extern "C" uintptr_t mm_lock_save(void) { assert(!locked); locked = true; return 0; }
extern "C" void mm_unlock_restore(uintptr_t flags) { assert(locked && !flags); locked = false; }
extern "C" void *test_detach(int sta, void *packet) { return bridge::sta_rx_detach(sta, packet); }
