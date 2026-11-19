/*
 * tone()/noTone() on the BL616CL general-purpose timers. A tone holds TIMER0 or TIMER1 in preload
 * mode counting the undivided crystal clock (XCLK, 40 MHz), and each comparator 0 match interrupt
 * toggles the pin. Every half period is exactly round(xclk / (2 * frequency)) counts; interrupt
 * latency moves single edges but does not accumulate. One pin sounds at a time.
 *
 * The timer registry for BLTimer (hw_timer.h) lives here so that a timed tone that has ended can
 * give its timer back to the next claim. LHAL updates the registers that TIMER0, TIMER1 and the
 * watchdog share (TCCR/TCDR/TCER/TCMR) read-modify-write, so only task context configures, starts
 * or stops a timer; the interrupt handler writes only this timer's TICR and TIER.
 */
#include "bflb_clock.h"
#include "bflb_gpio.h"
#include "bflb_irq.h"
#include "bflb_timer.h"
#include "hw_timer.h"
#include "pin_map.h"

#include "Arduino.h"

static constexpr unsigned int TONE_MAX_HZ = 20000;
static constexpr uint8_t TONE_NO_PIN = 0xFF;

static bool timer_held[2];

static struct bflb_device_s *tone_dev; /* Timer held by the tone, or nullptr. */
static struct bflb_device_s *tone_gpio_dev;
static uint8_t tone_pin = TONE_NO_PIN; /* Arduino pin of the held timer's tone. */
static uint8_t tone_gpio;
static volatile bool tone_high;
static volatile bool tone_timed;
static volatile bool tone_done;     /* Set by the interrupt when a timed tone ends. */
static volatile uint64_t tone_left; /* Matches left in a timed tone. */

struct bflb_device_s *hw_timer_claim(void)
{
    static const char *const names[] = {"timer0", "timer1"};

    if (tone_dev != nullptr && tone_done) {
        pin_tone_release(tone_pin); /* The pin already rests low. */
    }
    for (int i = 0; i < 2; i++) {
        if (!timer_held[i]) {
            timer_held[i] = true;
            return bflb_device_get_by_name(names[i]);
        }
    }
    return nullptr;
}

void hw_timer_release(struct bflb_device_s *timer)
{
    timer_held[timer->idx] = false;
}

/**
 * @brief End one half period with interrupts off: toggle the pin, or on the last match of a timed
 * tone drive it low and mask the match interrupt. The counter keeps running until task context
 * stops it.
 */
static void tone_step(void)
{
    if (tone_timed && --tone_left == 0) {
        bflb_timer_compint_mask(tone_dev, TIMER_COMP_ID_0, true);
        bflb_gpio_reset(tone_gpio_dev, tone_gpio);
        tone_done = true;
        return;
    }
    tone_high = !tone_high;
    if (tone_high) {
        bflb_gpio_set(tone_gpio_dev, tone_gpio);
    } else {
        bflb_gpio_reset(tone_gpio_dev, tone_gpio);
    }
}

/**
 * @brief Handle a comparator 0 match; ignore a stale IRQ without one.
 */
static void tone_isr(int irq, void *arg)
{
    (void)irq;
    (void)arg;
    if (bflb_timer_get_compint_status(tone_dev, TIMER_COMP_ID_0)) {
        bflb_timer_compint_clear(tone_dev, TIMER_COMP_ID_0);
        tone_step();
    }
}

/**
 * @brief Stop the counter, which clears it, and the match interrupt; drop a pending match.
 */
static void tone_halt(void)
{
    bflb_irq_disable(tone_dev->irq_num);
    bflb_timer_stop(tone_dev);
    bflb_timer_compint_mask(tone_dev, TIMER_COMP_ID_0, true);
    bflb_timer_compint_clear(tone_dev, TIMER_COMP_ID_0);
}

/**
 * @brief Matches until a timed tone ends, `counts` after the start of the current half period:
 * the nearest whole number of half periods, at least one.
 */
static uint64_t tone_matches(uint64_t counts, uint32_t half)
{
    uint64_t n = (counts + half / 2) / half;
    return n > 0 ? n : 1;
}

bool pin_tone_release(pin_size_t pin)
{
    if (tone_dev == nullptr || pin != tone_pin) {
        return false;
    }
    tone_halt();
    bflb_irq_detach(tone_dev->irq_num);
    hw_timer_release(tone_dev);
    tone_dev = nullptr;
    tone_pin = TONE_NO_PIN;
    return true;
}

/**
 * @brief Output a 50% square wave of 1–20000 Hz on a usable pin, for `duration` ms or until
 * noTone() when it is 0. Ignored, leaving the pin unchanged, for a reserved or invalid pin, an
 * out-of-range frequency, while another pin sounds, or when BLTimer holds both timers. On the
 * sounding pin it changes frequency and duration without restarting the wave: the half period in
 * progress ends at the new length, or at once if it has already lasted longer. Not for interrupt
 * context.
 */
void tone(uint8_t pin, unsigned int frequency, unsigned long duration)
{
    int gpio = pin_to_gpio(pin);
    if (gpio < 0 || frequency == 0 || frequency > TONE_MAX_HZ) {
        return;
    }
    bool sounding = tone_dev != nullptr && !tone_done;
    if (sounding && pin != tone_pin) {
        return; /* One tone at a time. */
    }
    uint32_t xclk = bflb_clk_get_system_clock(BFLB_SYSTEM_XCLK);
    uint32_t half = (xclk + frequency) / (2 * frequency); /* round(xclk / (2 * frequency)) */
    uint64_t counts = (uint64_t)duration * xclk / 1000;

    if (sounding) {
        uintptr_t flags = bflb_irq_save();
        if (!tone_done) {
            bflb_timer_set_compvalue(tone_dev, TIMER_COMP_ID_0, half - 2); /* Offset as in bflb_timer_init. */
            uint32_t now = bflb_timer_get_countervalue(tone_dev);
            tone_timed = duration > 0;
            if (now > half - 2) {
                /* Past the new match, where the counter would run on to 2^32: end this half period
                 * now and count the next from 0. */
                bflb_timer_stop(tone_dev);
                bflb_timer_start(tone_dev);
                tone_left = tone_matches(counts, half) + 1;
                tone_step();
            } else {
                tone_left = tone_matches(counts + now, half);
            }
            bflb_irq_restore(flags);
            return;
        }
        bflb_irq_restore(flags); /* It ended meanwhile: start a new tone below. */
    }

    if (tone_dev == nullptr) {
        struct bflb_device_s *dev = hw_timer_claim();
        if (dev == nullptr) {
            return;
        }
        tone_dev = dev;
        bflb_irq_attach(tone_dev->irq_num, tone_isr, nullptr);
    } else {
        tone_halt(); /* A timed tone that ended, on this pin or another. */
    }
    if (pin != tone_pin) {
        digitalWrite(pin, LOW); /* Stops analogWrite output; LOW is the level left after the tone. */
        pinMode(pin, OUTPUT);
        tone_pin = pin;
        tone_gpio = (uint8_t)gpio;
        tone_gpio_dev = bflb_device_get_by_name("gpio");
    }

    struct bflb_timer_config_s cfg = {};
    cfg.counter_mode = TIMER_COUNTER_MODE_PROLOAD;
    cfg.clock_source = TIMER_CLKSRC_XTAL;
    cfg.clock_div = 0;
    cfg.trigger_comp_id = TIMER_COMP_ID_0;
    cfg.comp0_val = half;
    cfg.comp1_val = 0xFFFFFFFFu;
    cfg.comp2_val = 0xFFFFFFFFu;
    cfg.preload_val = 0;
    bflb_timer_init(tone_dev, &cfg);
    tone_timed = duration > 0;
    tone_left = tone_matches(counts, half);
    tone_done = false;
    tone_high = true;
    bflb_gpio_set(tone_gpio_dev, tone_gpio);
    bflb_timer_compint_clear(tone_dev, TIMER_COMP_ID_0);
    bflb_timer_compint_mask(tone_dev, TIMER_COMP_ID_0, false);
    bflb_irq_enable(tone_dev->irq_num);
    bflb_timer_start(tone_dev);
}

/**
 * @brief Stop the tone on `pin` and leave it a digital output at LOW; no effect on other pins.
 */
void noTone(uint8_t pin)
{
    if (pin_tone_release(pin)) {
        digitalWrite(pin, LOW);
    }
}
