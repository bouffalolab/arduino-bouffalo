#pragma once

#include <stdint.h>

struct bflb_device_s;

/*
 * One BL616CL general-purpose timer (TIMER0 or TIMER1) counting 1 us ticks from the crystal.
 * Callbacks run in interrupt context. Call the methods from one task at a time; inside a callback
 * or another interrupt they fail without side effects.
 */
class BLTimer {
public:
    /** @brief Shortest accepted period or delay in microseconds. */
    static constexpr uint32_t MIN_PERIOD_US = 10;
    /** @brief Longest accepted period or delay in microseconds (about 71.6 minutes). */
    static constexpr uint32_t MAX_PERIOD_US = 0xFFFFFFFFu;

    BLTimer() = default;
    /** @brief Stop and release the hardware timer. */
    ~BLTimer();
    BLTimer(const BLTimer &) = delete;
    BLTimer &operator=(const BLTimer &) = delete;

    /** @brief Claim a free timer, or reuse this object's timer, and call callback every period_us. */
    bool begin(void (*callback)(), uint32_t period_us);
    /** @brief Same as begin(callback, period_us), passing arg to the callback. */
    bool begin(void (*callback)(void *), void *arg, uint32_t period_us);
    /** @brief Claim or reuse a timer and call callback once after delay_us. */
    bool beginOnce(void (*callback)(), uint32_t delay_us);
    /** @brief Same as beginOnce(callback, delay_us), passing arg to the callback. */
    bool beginOnce(void (*callback)(void *), void *arg, uint32_t delay_us);
    /** @brief Change the period or delay; a running timer restarts a full period from now. */
    bool setPeriod(uint32_t period_us);
    /** @brief Restart counting from zero; a one-shot timer is armed again. */
    bool start();
    /** @brief Stop counting and callbacks, keeping the timer and its configuration. */
    bool stop();
    /** @brief Stop and release the timer for other objects. Safe when nothing is held. */
    void end();
    /** @brief Return whether a callback is pending; false after a one-shot callback. */
    bool running() const { return running_; }
    /** @brief Return the held hardware timer, 0 or 1, or -1 when none is held. */
    int timer() const { return index_; }

private:
    bool configure(void (*plain)(), void (*with_arg)(void *), void *arg, uint32_t period_us, bool once);
    void restart();
    void halt();
    static void isr(int irq, void *arg);

    struct bflb_device_s *dev_ = nullptr;
    void (*plain_)() = nullptr;
    void (*with_arg_)(void *) = nullptr;
    void *arg_ = nullptr;
    int8_t index_ = -1;
    bool once_ = false;
    volatile bool running_ = false;
};
