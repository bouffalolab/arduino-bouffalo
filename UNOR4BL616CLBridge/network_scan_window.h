#ifndef UNOR4_BL616CL_NETWORK_SCAN_WINDOW_H
#define UNOR4_BL616CL_NETWORK_SCAN_WINDOW_H
#include <stdint.h>
#include <atomic>

namespace bridge {

// A controller-owned RF window; acquire only for actual network work and
// restore on every exit. Missing callbacks leave the original path intact.
class NetworkScanWindow {
public:
    using Pause = bool (*)(void *context);
    using Resume = void (*)(void *context);

    NetworkScanWindow(Pause pause, Resume resume, void *context)
        : resume_(resume), context_(context), paused_(
              pause != nullptr && resume != nullptr && pause(context)) {
        if (paused_) generation().fetch_add(1, std::memory_order_relaxed);
    }
    // An idle receive service can reuse RF opportunities from
    // TCP/TLS/UDP operations instead of pausing again between their commands.
    // AP startup can also acquire a window from its worker task. The sequence
    // carries no payload; relaxed atomic access avoids a cross-task data race.
    static uint32_t activity_generation() { return generation().load(std::memory_order_relaxed); }
    bool paused() const { return paused_; }
    ~NetworkScanWindow() { if (paused_) resume_(context_); }
    NetworkScanWindow(const NetworkScanWindow &) = delete;
    NetworkScanWindow &operator=(const NetworkScanWindow &) = delete;

private:
    static std::atomic<uint32_t> &generation() {
        static std::atomic<uint32_t> value{0}; return value;
    }
    Resume resume_;
    void *context_;
    bool paused_;
};

} // namespace bridge

#endif
