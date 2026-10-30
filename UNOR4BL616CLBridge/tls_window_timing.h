#pragma once
#include <stdint.h>

#ifndef BL616CL_TLS_WINDOW_TIMING
#define BL616CL_TLS_WINDOW_TIMING 0
#endif

namespace bridge {
#if BL616CL_TLS_WINDOW_TIMING
// AT-task owned. No reset, allocation, UART output or controller callback work.
struct TlsWindowTimingGroup {
    uint32_t count = 0, paused = 0;
    uint32_t sum[3] = {}, maximum[3] = {};
};
struct TlsWindowTimingCounters {
    uint32_t overflow = 0;
    TlsWindowTimingGroup groups[3]; // available, peek, receive
};
inline TlsWindowTimingCounters &tls_window_timing_counters()
{
    static TlsWindowTimingCounters counters;
    return counters;
}
class TlsWindowTiming {
public:
    using Clock = uint32_t (*)();
    TlsWindowTiming(int operation, Clock clock)
        : operation_(operation), clock_(clock), begin_(operation >= 0 ? clock() : 0) {}
    void pause_finished(bool paused) {
        if (operation_ >= 0) { paused_ = paused; pause_end_ = clock_(); }
    }
    class Body {
    public:
        explicit Body(TlsWindowTiming &owner) : owner_(owner) {}
        ~Body() { if (owner_.operation_ >= 0) owner_.body_end_ = owner_.clock_(); }
    private:
        TlsWindowTiming &owner_;
    };
    ~TlsWindowTiming() {
        if (operation_ < 0 || operation_ >= 3) return;
        const uint32_t end = clock_();
        auto &counters = tls_window_timing_counters();
        auto &group = counters.groups[operation_];
        add(group.count, 1, counters.overflow);
        if (paused_) add(group.paused, 1, counters.overflow);
        const uint32_t elapsed[] = {pause_end_ - begin_, body_end_ - pause_end_, end - body_end_};
        for (unsigned i = 0; i < 3; ++i) {
            add(group.sum[i], elapsed[i], counters.overflow);
            if (elapsed[i] > group.maximum[i]) group.maximum[i] = elapsed[i];
        }
    }
private:
    static void add(uint32_t &value, uint32_t amount, uint32_t &overflow) {
        if (UINT32_MAX - value < amount) { value = UINT32_MAX; overflow = 1; }
        else value += amount;
    }
    int operation_;
    Clock clock_;
    uint32_t begin_, pause_end_ = 0, body_end_ = 0;
    bool paused_ = false;
};
#endif
}
