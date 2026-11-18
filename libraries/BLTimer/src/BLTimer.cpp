/*
 * TIMER0/TIMER1 run in preload mode: comparator 0 matches, the counter reloads 0 and raises the
 * interrupt. Disabling a timer resets its counter to 0 (measured on BL616CL; lhal stop and
 * bflb_timer_init clear only the enable bit), so every start begins a full period.
 * TCCR/TCDR/TCER/TCMR are shared by both timers (TCCR/TCDR also by the watchdog) and lhal updates
 * them read-modify-write, so only task context touches them. The interrupt handler writes only
 * this timer's TICR and TIER; a one-shot timer keeps counting with its interrupt masked. The
 * shared TIMER0/1/WDG clock gate is enabled by board_init and never disabled here.
 */
#include "FreeRTOS.h"
#include "bflb_clock.h"
#include "bflb_irq.h"
#include "bflb_timer.h"

#include "BLTimer.h"

static const char *const timer_names[] = { "timer0", "timer1" };
static BLTimer *owners[2];

/**
 * @brief Stop and release the hardware timer.
 */
BLTimer::~BLTimer()
{
    end();
}

/**
 * @brief Claim a free timer, or reuse this object's timer, and call callback every period_us.
 */
bool BLTimer::begin(void (*callback)(), uint32_t period_us)
{
    return callback != nullptr && configure(callback, nullptr, nullptr, period_us, false);
}

/**
 * @brief Same as begin(callback, period_us), passing arg to the callback.
 */
bool BLTimer::begin(void (*callback)(void *), void *arg, uint32_t period_us)
{
    return callback != nullptr && configure(nullptr, callback, arg, period_us, false);
}

/**
 * @brief Claim or reuse a timer and call callback once after delay_us.
 */
bool BLTimer::beginOnce(void (*callback)(), uint32_t delay_us)
{
    return callback != nullptr && configure(callback, nullptr, nullptr, delay_us, true);
}

/**
 * @brief Same as beginOnce(callback, delay_us), passing arg to the callback.
 */
bool BLTimer::beginOnce(void (*callback)(void *), void *arg, uint32_t delay_us)
{
    return callback != nullptr && configure(nullptr, callback, arg, delay_us, true);
}

/**
 * @brief Validate the request, claim a timer when none is held, program 1 us ticks from the
 * crystal and start. A rejected request leaves the object unchanged.
 */
bool BLTimer::configure(void (*plain)(), void (*with_arg)(void *), void *arg, uint32_t period_us, bool once)
{
    /* The divider makes 1 MHz from XCLK (40 MHz on supported boards); it must divide exactly. */
    uint32_t xclk = bflb_clk_get_system_clock(BFLB_SYSTEM_XCLK);
    if (xPortIsInsideInterrupt() || period_us < MIN_PERIOD_US || xclk % 1000000 != 0 ||
        xclk < 1000000 || xclk > 256000000) {
        return false;
    }
    if (dev_ == nullptr) {
        int index = owners[0] == nullptr ? 0 : owners[1] == nullptr ? 1 : -1;
        struct bflb_device_s *dev = index < 0 ? nullptr : bflb_device_get_by_name(timer_names[index]);
        if (dev == nullptr) {
            return false;
        }
        owners[index] = this;
        index_ = (int8_t)index;
        dev_ = dev;
        bflb_irq_attach(dev_->irq_num, isr, this);
    }
    halt();
    plain_ = plain;
    with_arg_ = with_arg;
    arg_ = arg;
    once_ = once;

    /* lhal stores compN_val - 2 and halts on values below 2; unused comparators get the maximum. */
    struct bflb_timer_config_s cfg = {};
    cfg.counter_mode = TIMER_COUNTER_MODE_PROLOAD;
    cfg.clock_source = TIMER_CLKSRC_XTAL;
    cfg.clock_div = (uint8_t)(xclk / 1000000 - 1);
    cfg.trigger_comp_id = TIMER_COMP_ID_0;
    cfg.comp0_val = period_us;
    cfg.comp1_val = 0xFFFFFFFFu;
    cfg.comp2_val = 0xFFFFFFFFu;
    cfg.preload_val = 0;
    bflb_timer_init(dev_, &cfg);
    restart();
    return true;
}

/**
 * @brief Change the period or delay; a running timer restarts a full period from now.
 */
bool BLTimer::setPeriod(uint32_t period_us)
{
    if (xPortIsInsideInterrupt() || dev_ == nullptr || period_us < MIN_PERIOD_US) {
        return false;
    }
    bool was_running = running_;
    halt();
    bflb_timer_set_compvalue(dev_, TIMER_COMP_ID_0, period_us - 2); /* Same offset as bflb_timer_init. */
    if (was_running) {
        restart();
    }
    return true;
}

/**
 * @brief Restart counting from zero; a one-shot timer is armed again.
 */
bool BLTimer::start()
{
    if (xPortIsInsideInterrupt() || dev_ == nullptr) {
        return false;
    }
    halt();
    restart();
    return true;
}

/**
 * @brief Stop counting and callbacks, keeping the timer and its configuration.
 */
bool BLTimer::stop()
{
    if (xPortIsInsideInterrupt() || dev_ == nullptr) {
        return false;
    }
    halt();
    return true;
}

/**
 * @brief Stop and release the timer for other objects. Safe when nothing is held.
 */
void BLTimer::end()
{
    if (xPortIsInsideInterrupt() || dev_ == nullptr) {
        return;
    }
    halt();
    bflb_irq_detach(dev_->irq_num);
    owners[index_] = nullptr;
    dev_ = nullptr;
    index_ = -1;
}

/**
 * @brief Stop the counter and its interrupt and discard a pending match; the IRQ stays disabled
 * until restart().
 */
void BLTimer::halt()
{
    bflb_irq_disable(dev_->irq_num);
    bflb_timer_stop(dev_);
    bflb_timer_compint_mask(dev_, TIMER_COMP_ID_0, true);
    bflb_timer_compint_clear(dev_, TIMER_COMP_ID_0);
    running_ = false;
}

/**
 * @brief Start a full period on the halted timer, whose counter is 0, with its interrupt enabled.
 */
void BLTimer::restart()
{
    bflb_timer_compint_clear(dev_, TIMER_COMP_ID_0);
    bflb_timer_compint_mask(dev_, TIMER_COMP_ID_0, false);
    running_ = true;
    bflb_irq_enable(dev_->irq_num);
    bflb_timer_start(dev_);
}

/**
 * @brief Acknowledge a comparator 0 match, mask a one-shot timer, then run the callback. A stale
 * IRQ without a pending match is ignored.
 */
void BLTimer::isr(int irq, void *arg)
{
    (void)irq;
    BLTimer *self = static_cast<BLTimer *>(arg);
    if (!bflb_timer_get_compint_status(self->dev_, TIMER_COMP_ID_0)) {
        return;
    }
    bflb_timer_compint_clear(self->dev_, TIMER_COMP_ID_0);
    if (self->once_) {
        bflb_timer_compint_mask(self->dev_, TIMER_COMP_ID_0, true);
        self->running_ = false;
    }
    if (self->with_arg_ != nullptr) {
        self->with_arg_(self->arg_);
    } else {
        self->plain_();
    }
}
