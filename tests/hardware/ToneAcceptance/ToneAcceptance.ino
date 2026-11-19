/*
 * tone()/noTone() hardware acceptance probe for Ai-M64P-32S-Kit. It needs the board's jumpers
 * GPIO8-GPIO22, GPIO14-GPIO15, GPIO18-GPIO19 and GPIO20-GPIO23. Serial carries line commands from
 * tests/hardware/tone_acceptance.py at 115200 8N1; every command answers with "RESULT ..." lines
 * that the host script judges.
 *
 * A tone toggles its pin from a timer interrupt, so the probe polls the GPIO input register of the
 * jumpered partner with interrupts on and timestamps every edge with the CPU cycle counter. An
 * edge is seen when the tone interrupt returns, or later when another interrupt delays the poll,
 * so the measured edge times include those delays. The CPU clock and the 40 MHz timer clock come
 * from the same crystal: frequency checks confirm the timer configuration, not the crystal.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <BLTimer.h>

#include "bflb_clock.h"

static const uint32_t GLB_BASE_ADDR = 0x20000000;
static const uint32_t GPIO_CFG0 = GLB_BASE_ADDR + 0x8C4; /* One 32-bit config word per GPIO. */
static const uint32_t GPIO_IN = GLB_BASE_ADDR + 0xAC4;   /* Input levels of GPIO0-31. */
static const uint32_t TIMER_TCER = 0x2000A500 + 0x84;    /* TIMER_BASE + TCER: TIMER0/1 enable bits 1/2. */
/* Config bits that follow the pad level or interrupt state rather than the configuration. */
static const uint32_t CFG_VOLATILE = (1u << 28) | (1u << 21);
static const uint8_t NUM_GPIO = 37;

static char line_buf[160];
static size_t line_len;

/**
 * @brief Print one formatted line on Serial.
 */
static void reply(const char *fmt, ...)
{
    char buf[480];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.println(buf);
}

static inline uint32_t reg(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile("csrr %0, mcycle" : "=r"(c));
    return c;
}

static uint32_t cpu_hz(void)
{
    return bflb_clk_get_system_clock(BFLB_SYSTEM_CPU_CLK);
}

static uint32_t cfg(uint8_t gpio)
{
    return reg(GPIO_CFG0 + 4 * gpio) & ~CFG_VOLATILE;
}

/* Report a pin's function, output enable, output bit and pad level. */
static void report_cfg(const char *tag, uint8_t gpio)
{
    uint32_t w = reg(GPIO_CFG0 + 4 * gpio);
    reply("RESULT %s pin=%u func=%lu oe=%lu o=%lu i=%lu ie=%lu pu=%lu pd=%lu", tag, gpio, (w >> 8) & 0x1f,
          (w >> 6) & 1, (w >> 24) & 1, (w >> 28) & 1, w & 1, (w >> 4) & 1, (w >> 5) & 1);
}

/* Return every GPIO0-31 to a floating input, which also stops a tone and frees PWM channels. */
static void release_all(void)
{
    for (uint8_t p = 0; p < 32; p++) {
        pinMode(p, INPUT);
    }
}

/* Release every pin and pull the sampled input down, so a stopped output reads low. */
static void prepare(uint8_t in)
{
    release_all();
    pinMode(in, INPUT_PULLDOWN);
}

/* --- Edge capture ------------------------------------------------------------------------- */

struct Edge {
    uint32_t t;
    uint32_t v;
};
static Edge edges[4096];

typedef void (*Action)(void);

/* Result of one capture window; times are CPU cycles from the window start. */
struct Capture {
    uint32_t n, v0, vend, iters, win, call, poll_gap;
    bool overflow;
};

/* Declared here so the Arduino prototype generator does not place them before Capture. */
static Capture capture(uint32_t mask, uint32_t ms, Action start);
static Capture tone_capture(uint8_t out, uint8_t in, unsigned int hz, unsigned long dur, uint32_t ms);

static Action every_action;
static uint32_t every_cycles, every_until;

/**
 * @brief Read the masked inputs, run `start` once (its duration is `call`), then poll GPIO_IN with
 * interrupts on until `ms` have passed since the first read. While a periodic action is set, run
 * it every every_cycles until every_until. Records each change of the masked bits.
 */
static Capture capture(uint32_t mask, uint32_t ms, Action start)
{
    Capture c = {};
    c.win = cpu_hz() / 1000 * ms;
    uint32_t t0 = cycles();
    c.v0 = reg(GPIO_IN) & mask;
    if (start != nullptr) {
        start();
    }
    uint32_t t = cycles();
    c.call = t - t0;
    uint32_t prev = c.v0, last = t, next_every = c.call + every_cycles;
    uint32_t k = 0, it = 0, gap = 0;
    do {
        uint32_t v = reg(GPIO_IN) & mask;
        t = cycles();
        it++;
        gap = t - last > gap ? t - last : gap;
        last = t;
        if (v != prev) {
            if (k < sizeof(edges) / sizeof(edges[0])) {
                edges[k].t = t - t0;
                edges[k].v = v;
                k++;
            } else {
                c.overflow = true;
            }
            prev = v;
        }
        if (every_action != nullptr && t - t0 >= next_every && t - t0 < every_until) {
            every_action();
            next_every += every_cycles;
            last = cycles();
        }
    } while (t - t0 < c.win);
    c.n = k;
    c.vend = prev;
    c.iters = it;
    c.poll_gap = gap;
    return c;
}

/* Edge statistics of one input in a capture: periods rise to rise, high rise to fall, low fall to
 * rise, the longest interval between edges, and the first and last edges. */
struct Wave {
    uint32_t rises, falls, lvl0, lvl1, first_edge, first_rise, first_fall, last_edge, last_fall, edge_gap;
    uint64_t per_sum, hi_sum, lo_sum;
    uint32_t per_n, hi_n, lo_n, per_min, per_max, hi_min, hi_max, lo_min, lo_max;
};

/* Declared here so the Arduino prototype generator does not place them before Wave. */
static Wave analyze(uint8_t pin, const Capture &c);
static void report(const char *tag, uint8_t pin, const Capture &c);

static void stat(uint32_t x, uint64_t &sum, uint32_t &n, uint32_t &mn, uint32_t &mx)
{
    sum += x;
    n++;
    mn = x < mn ? x : mn;
    mx = x > mx ? x : mx;
}

static Wave analyze(uint8_t pin, const Capture &c)
{
    Wave w = {};
    w.per_min = w.hi_min = w.lo_min = UINT32_MAX;
    w.lvl0 = (c.v0 >> pin) & 1;
    w.lvl1 = (c.vend >> pin) & 1;
    uint32_t last = w.lvl0, t_rise = 0, t_fall = 0, t_edge = 0;
    bool have_rise = false, have_fall = false, have_edge = false;
    for (uint32_t i = 0; i < c.n; i++) {
        uint32_t lv = (edges[i].v >> pin) & 1;
        if (lv == last) {
            continue;
        }
        last = lv;
        uint32_t t = edges[i].t;
        if (have_edge) {
            w.edge_gap = t - t_edge > w.edge_gap ? t - t_edge : w.edge_gap;
        } else {
            w.first_edge = t;
        }
        have_edge = true;
        t_edge = t;
        w.last_edge = t;
        if (lv) {
            if (have_rise) {
                stat(t - t_rise, w.per_sum, w.per_n, w.per_min, w.per_max);
            } else {
                w.first_rise = t;
            }
            if (have_fall) {
                stat(t - t_fall, w.lo_sum, w.lo_n, w.lo_min, w.lo_max);
            }
            w.rises++;
            have_rise = true;
            t_rise = t;
        } else {
            if (have_rise) {
                stat(t - t_rise, w.hi_sum, w.hi_n, w.hi_min, w.hi_max);
            }
            if (!have_fall) {
                w.first_fall = t;
            }
            w.falls++;
            w.last_fall = t;
            have_fall = true;
            t_fall = t;
        }
    }
    w.per_min = w.per_n ? w.per_min : 0;
    w.hi_min = w.hi_n ? w.hi_min : 0;
    w.lo_min = w.lo_n ? w.lo_min : 0;
    return w;
}

static void report(const char *tag, uint8_t pin, const Capture &c)
{
    Wave w = analyze(pin, c);
    /* Sums stay within 32 bits for windows up to 13 s. */
    reply("RESULT %s in=%u rises=%lu falls=%lu lvl0=%lu lvl1=%lu per_sum=%lu per_n=%lu per_min=%lu per_max=%lu "
          "hi_sum=%lu hi_n=%lu hi_min=%lu hi_max=%lu lo_sum=%lu lo_n=%lu lo_min=%lu lo_max=%lu",
          tag, pin, w.rises, w.falls, w.lvl0, w.lvl1, (uint32_t)w.per_sum, w.per_n, w.per_min, w.per_max,
          (uint32_t)w.hi_sum, w.hi_n, w.hi_min, w.hi_max, (uint32_t)w.lo_sum, w.lo_n, w.lo_min, w.lo_max);
    reply("RESULT %s.t in=%u first_edge=%lu first_rise=%lu first_fall=%lu last_edge=%lu last_fall=%lu edge_gap=%lu "
          "call=%lu win=%lu iters=%lu poll_gap=%lu overflow=%u cpu=%lu",
          tag, pin, w.first_edge, w.first_rise, w.first_fall, w.last_edge, w.last_fall, w.edge_gap, c.call, c.win,
          c.iters, c.poll_gap, c.overflow, cpu_hz());
}

/* --- Actions run at the start of a capture ------------------------------------------------ */

static uint8_t a_pin;
static unsigned int a_hz;
static unsigned long a_dur;
static int a_level;

static void act_tone(void)
{
    tone(a_pin, a_hz, a_dur);
}

static void act_notone(void)
{
    noTone(a_pin);
}

static void act_digital_write(void)
{
    digitalWrite(a_pin, a_level ? HIGH : LOW);
}

static void act_input(void)
{
    pinMode(a_pin, INPUT);
}

static void act_analog_write(void)
{
    analogWrite(a_pin, a_level);
}

/* Start a tone on `out` and sample its partner `in` from the call. */
static Capture tone_capture(uint8_t out, uint8_t in, unsigned int hz, unsigned long dur, uint32_t ms)
{
    a_pin = out;
    a_hz = hz;
    a_dur = dur;
    return capture(1u << in, ms, act_tone);
}

/* Busy-wait `us` microseconds from cycle count t0. */
static void wait_from(uint32_t t0, uint32_t us)
{
    uint32_t n = cpu_hz() / 1000000 * us;
    while (cycles() - t0 < n) {
    }
}

static volatile uint32_t timer_ticks;

static void on_tick()
{
    timer_ticks++;
}

/* Whether two BLTimer objects can claim both timers now; reports their indices and releases them. */
static bool both_timers_free(const char *tag)
{
    BLTimer x, y;
    bool a = x.begin(on_tick, 1000), b = y.begin(on_tick, 1000);
    reply("RESULT %s first=%d second=%d tcer=0x%lx", tag, x.timer(), y.timer(), reg(TIMER_TCER));
    return a && b;
}

/* --- Commands ----------------------------------------------------------------------------- */

static void cmd_info(void)
{
    reply("RESULT INFO cpu=%lu xclk=%lu", cpu_hz(), bflb_clk_get_system_clock(BFLB_SYSTEM_XCLK));
}

/* Drive each jumpered pin both ways and read its partner. */
static void cmd_shorts(void)
{
    static const uint8_t pairs[][2] = {{8, 22}, {22, 8}, {14, 15}, {15, 14}, {18, 19}, {19, 18}, {20, 23}, {23, 20}};
    release_all();
    for (const auto &p : pairs) {
        pinMode(p[1], INPUT);
        pinMode(p[0], OUTPUT);
        digitalWrite(p[0], HIGH);
        delayMicroseconds(20);
        int hi = digitalRead(p[1]);
        digitalWrite(p[0], LOW);
        delayMicroseconds(20);
        int lo = digitalRead(p[1]);
        pinMode(p[0], INPUT);
        reply("RESULT SHORT out=%u in=%u hi=%d lo=%d", p[0], p[1], hi, lo);
    }
}

/* FREQ out in hz ms stop_ms: sound until noTone(), then check that the output stops low. */
static void cmd_freq(uint8_t out, uint8_t in, unsigned int hz, uint32_t ms, uint32_t stop_ms)
{
    prepare(in);
    report("FREQ", in, tone_capture(out, in, hz, 0, ms));
    report_cfg("FREQCFG", out);
    a_pin = out;
    report("FREQSTOP", in, capture(1u << in, stop_ms, act_notone));
    report_cfg("FREQSTOPCFG", out);
}

/* DUR out in hz dur ms: a timed tone must stop by itself and leave the pin a low output. */
static void cmd_dur(uint8_t out, uint8_t in, unsigned int hz, unsigned long dur, uint32_t ms)
{
    prepare(in);
    report("DUR", in, tone_capture(out, in, hz, dur, ms));
    report_cfg("DURCFG", out);
}

static void cmd_notone(void)
{
    prepare(22);
    /* Stop while the output is high, 20 ms into the first 50 ms half period of 10 Hz. */
    report("NT_HIGH", 22, tone_capture(8, 22, 10, 0, 20));
    a_pin = 8;
    report("NT_STOP", 22, capture(1u << 22, 200, act_notone));
    report_cfg("NT_STOPCFG", 8);
    reply("RESULT NT_TIMERS ok=%d", both_timers_free("NT_TIMERS_CLAIM"));

    /* noTone() on another pin leaves that pin and the running tone alone. */
    pinMode(19, INPUT_PULLUP);
    tone(8, 1000);
    uint32_t before = cfg(19);
    noTone(19);
    reply("RESULT NT_OTHER same=%d", cfg(19) == before);
    report("NT_OTHER_WAVE", 22, capture(1u << 22, 25, nullptr));
    noTone(8);

    /* Without any tone, noTone() changes nothing. */
    pinMode(8, INPUT_PULLUP);
    before = cfg(8);
    noTone(8);
    reply("RESULT NT_IDLE same=%d", cfg(8) == before);
    release_all();
}

static void cmd_one(void)
{
    release_all();
    tone(8, 1000);
    uint32_t before = cfg(18);
    tone(18, 2000); /* Another pin while 8 sounds: no effect. */
    reply("RESULT ONE_OTHER same=%d", cfg(18) == before);
    Capture c = capture((1u << 22) | (1u << 19), 25, nullptr);
    report("ONE_A", 22, c);
    report("ONE_A", 19, c);
    a_pin = 8;
    a_hz = 2000;
    a_dur = 0;
    report("ONE_SAME", 22, capture(1u << 22, 25, act_tone)); /* Same pin: new frequency. */
    noTone(8);
    tone(18, 2000);
    c = capture((1u << 22) | (1u << 19), 25, nullptr);
    report("ONE_NEXT", 22, c);
    report("ONE_NEXT", 19, c);
    noTone(18);
    tone(8, 1000, 5);
    delay(10); /* The timed tone has ended, so another pin may sound. */
    tone(18, 3000);
    report("ONE_AFTER", 19, capture(1u << 19, 25, nullptr));
    release_all();
}

/* CHANGE f1 wait_us f2 dur ms: change a sounding pin's frequency wait_us after it started. */
static void cmd_change(unsigned int f1, uint32_t wait_us, unsigned int f2, unsigned long dur, uint32_t ms)
{
    prepare(22);
    uint32_t t0 = cycles();
    tone(8, f1);
    uint32_t started = cycles() - t0; /* The counter starts at the end of tone(). */
    wait_from(t0, wait_us);
    uint32_t t1 = cycles();
    Capture c = tone_capture(8, 22, f2, dur, ms);
    reply("RESULT CHANGE_SINCE since=%lu started=%lu", t1 - t0, started);
    report("CHANGE", 22, c);
    release_all();
}

static unsigned int pitch_a, pitch_b;
static bool pitch_flip;

static void act_pitch(void)
{
    pitch_flip = !pitch_flip;
    tone(8, pitch_flip ? pitch_a : pitch_b, 10);
}

/* PITCH ms f1 f2: tone(8, f, 10) every millisecond for `ms`, as tonePitchFollower does with a
 * noisy sensor, then 40 ms more for the last 10 ms tone to end. */
static void cmd_pitch(uint32_t ms, unsigned int f1, unsigned int f2)
{
    prepare(22);
    pitch_a = f1;
    pitch_b = f2;
    pitch_flip = false;
    every_action = act_pitch;
    every_cycles = cpu_hz() / 1000;
    every_until = cpu_hz() / 1000 * ms;
    Capture c = capture(1u << 22, ms + 40, act_pitch);
    every_action = nullptr;
    report("PITCH", 22, c);
    release_all();
}

static void cmd_busy(void)
{
    release_all();
    BLTimer ta, tb, tc;
    bool a = ta.begin(on_tick, 1000), b = tb.begin(on_tick, 1000);
    uint32_t before = cfg(8);
    a_pin = 8;
    a_hz = 1000;
    a_dur = 0;
    Capture c = capture(1u << 22, 25, act_tone); /* Both timers held: no tone. */
    reply("RESULT BUSY_FULL a=%d b=%d same=%d tcer=0x%lx", a, b, cfg(8) == before, reg(TIMER_TCER));
    report("BUSY_FULL_WAVE", 22, c);

    tb.end();
    report("BUSY_FREE_WAVE", 22, capture(1u << 22, 25, act_tone));
    bool c1 = tc.begin(on_tick, 1000); /* TIMER0 by ta, the other by the tone. */
    noTone(8);
    bool c2 = tc.begin(on_tick, 1000);
    reply("RESULT BUSY_SHARE while_tone=%d after_notone=%d timer=%d", c1, c2, tc.timer());
    tc.end();

    tone(8, 1000, 5);
    delay(20); /* Ended without noTone(): the next claim takes the timer back. */
    bool c3 = tc.begin(on_tick, 1000);
    before = cfg(8);
    c = capture(1u << 22, 25, act_tone); /* ta and tc hold both timers again. */
    reply("RESULT BUSY_RECLAIM claim=%d timer=%d same=%d", c3, tc.timer(), cfg(8) == before);
    report_cfg("BUSY_RECLAIM_CFG", 8);
    report("BUSY_RECLAIM_WAVE", 22, c);
    ta.end();
    tc.end();
    release_all();
}

static void cmd_invalid(void)
{
    static const uint8_t bad_pins[] = {32, 33, 36, 37, 40, 100, 255};
    static const unsigned int bad_hz[] = {0, 20001, 65535, 4294967295u};
    static uint32_t snap[NUM_GPIO];
    release_all();
    for (uint8_t g = 0; g < NUM_GPIO; g++) {
        snap[g] = cfg(g);
    }
    uint32_t tcer = reg(TIMER_TCER);
    for (uint8_t p : bad_pins) {
        tone(p, 1000);
        noTone(p);
    }
    for (unsigned int hz : bad_hz) {
        tone(8, hz);
    }
    uint32_t changed = 0;
    for (uint8_t g = 0; g < NUM_GPIO; g++) {
        changed += cfg(g) != snap[g];
    }
    reply("RESULT INV_IDLE changed=%lu tcer_same=%d", changed, reg(TIMER_TCER) == tcer);
    reply("RESULT INV_TIMERS ok=%d", both_timers_free("INV_TIMERS_CLAIM"));

    tone(8, 1000);
    for (unsigned int hz : bad_hz) {
        tone(8, hz); /* Ignored: the 1 kHz tone keeps running. */
    }
    for (uint8_t p : bad_pins) {
        tone(p, 2000);
    }
    report("INV_RUNNING", 22, capture(1u << 22, 25, nullptr));
    release_all();
}

static void cmd_pwm(void)
{
    release_all();
    analogWrite(18, 128); /* CH2 */
    analogWrite(20, 64);  /* CH0 */
    tone(15, 3000);       /* GPIO15 is a CH3 pin, free here. */
    Capture c = capture((1u << 19) | (1u << 23) | (1u << 14), 25, nullptr);
    report("PWM_A", 19, c);
    report("PWM_A", 23, c);
    report("PWM_A", 14, c);

    a_pin = 15;
    a_level = 191;
    report("PWM_BX", 14, capture(1u << 14, 3, act_analog_write)); /* Tone pin to PWM. */
    c = capture((1u << 19) | (1u << 23) | (1u << 14), 25, nullptr);
    report("PWM_B", 19, c);
    report("PWM_B", 23, c);
    report("PWM_B", 14, c);
    report_cfg("PWM_BCFG", 15);
    reply("RESULT PWM_B_TIMERS ok=%d", both_timers_free("PWM_B_CLAIM"));

    tone(8, 2000);
    analogWrite(8, 100); /* CH0 belongs to GPIO20: refused, so the tone goes on. */
    report("PWM_C", 22, capture(1u << 22, 25, nullptr));
    report_cfg("PWM_CCFG", 8);
    noTone(8);

    tone(18, 2000); /* PWM pin to tone. */
    c = capture((1u << 19) | (1u << 23), 25, nullptr);
    report("PWM_D", 19, c);
    report("PWM_D", 23, c);
    report_cfg("PWM_DCFG", 18);

    analogWrite(18, 64); /* And back to PWM on CH2. */
    delay(3);            /* The first period can still use the channel's previous threshold. */
    c = capture((1u << 19) | (1u << 23), 25, nullptr);
    report("PWM_E", 19, c);
    report("PWM_E", 23, c);
    release_all();
}

/* TIMERCO hz ms: a 100 us BLTimer and a tone at once. */
static void cmd_timerco(unsigned int hz, uint32_t ms)
{
    prepare(22);
    BLTimer t;
    bool ok = t.begin(on_tick, 100);
    tone(8, hz);
    uint32_t n0 = timer_ticks;
    Capture c = capture(1u << 22, ms, nullptr);
    uint32_t n1 = timer_ticks;
    int index = t.timer();
    t.end();
    reply("RESULT TCO_TIMER ok=%d timer=%d ticks=%lu win=%lu", ok, index, n1 - n0, c.win);
    report("TCO", 22, c);
    release_all();
}

static void cmd_digital(void)
{
    release_all();
    tone(8, 1000);
    delay(3);
    a_pin = 8;
    a_level = 1;
    report("DIG_WRITE", 22, capture(1u << 22, 20, act_digital_write));
    report_cfg("DIG_WRITECFG", 8);
    reply("RESULT DIG_WRITE_TIMERS ok=%d", both_timers_free("DIG_WRITE_CLAIM"));

    release_all();
    pinMode(22, INPUT_PULLDOWN); /* Holds the jumper low once GPIO8 stops driving. */
    tone(8, 1000);
    delay(3);
    report("DIG_INPUT", 22, capture(1u << 22, 20, act_input));
    report_cfg("DIG_INPUTCFG", 8);
    reply("RESULT DIG_INPUT_TIMERS ok=%d", both_timers_free("DIG_INPUT_CLAIM"));

    /* GPIO2 (A2) has no jumper: watch its own input bit, then analogRead() it. */
    release_all();
    tone(2, 1000);
    report("DIG_ADC_TONE", 2, capture(1u << 2, 10, nullptr));
    int v = analogRead(2);
    report_cfg("DIG_ADCCFG", 2);
    reply("RESULT DIG_ADC value=%d", v);
    reply("RESULT DIG_ADC_TIMERS ok=%d", both_timers_free("DIG_ADC_CLAIM"));
    release_all();
}

/* LOAD: poll rate with a 1 Hz tone (no interrupt in the window) and a 20 kHz tone, alternately, on the
 * same pin and code path; the difference is the CPU share of the 20 kHz interrupts. */
static void cmd_load(void)
{
    static const unsigned int hz[] = {1, 20000, 1, 20000, 1, 20000};
    prepare(22);
    for (unsigned int f : hz) {
        tone(8, f);
        delay(5);
        Capture c = capture(1u << 22, 50, nullptr);
        reply("RESULT LOAD hz=%u iters=%lu win=%lu", f, c.iters, c.win);
        noTone(8);
    }
    release_all();
}

/* The official toneMelody sequence on pin 8, with the inter-note delay spent sampling GPIO22. */
static void cmd_melody(void)
{
    static const unsigned int melody[] = {262, 196, 196, 220, 196, 0, 247, 262}; /* pitches.h values */
    static const int note_durations[] = {4, 8, 8, 4, 4, 4, 4, 4};
    prepare(22);
    for (int i = 0; i < 8; i++) {
        int note_duration = 1000 / note_durations[i];
        int pause = note_duration * 1.30;
        char tag[12];
        snprintf(tag, sizeof(tag), "MEL%d", i);
        report(tag, 22, tone_capture(8, 22, melody[i], note_duration, pause));
        noTone(8);
    }
    release_all();
}

static void dispatch(char *line)
{
    char *argv[7];
    int argc = 0;
    for (char *tok = strtok(line, " "); tok && argc < 7; tok = strtok(nullptr, " ")) {
        argv[argc++] = tok;
    }
    if (argc == 0) {
        return;
    }
    unsigned long a[6] = {};
    for (int i = 1; i < argc; i++) {
        a[i - 1] = strtoul(argv[i], nullptr, 0);
    }
    if (!strcmp(argv[0], "INFO")) {
        cmd_info();
    } else if (!strcmp(argv[0], "SHORTS")) {
        cmd_shorts();
    } else if (!strcmp(argv[0], "FREQ") && argc == 6) {
        cmd_freq(a[0], a[1], a[2], a[3], a[4]);
    } else if (!strcmp(argv[0], "DUR") && argc == 6) {
        cmd_dur(a[0], a[1], a[2], a[3], a[4]);
    } else if (!strcmp(argv[0], "NOTONE")) {
        cmd_notone();
    } else if (!strcmp(argv[0], "ONE")) {
        cmd_one();
    } else if (!strcmp(argv[0], "CHANGE") && argc == 6) {
        cmd_change(a[0], a[1], a[2], a[3], a[4]);
    } else if (!strcmp(argv[0], "PITCH") && argc == 4) {
        cmd_pitch(a[0], a[1], a[2]);
    } else if (!strcmp(argv[0], "BUSY")) {
        cmd_busy();
    } else if (!strcmp(argv[0], "INVALID")) {
        cmd_invalid();
    } else if (!strcmp(argv[0], "PWM")) {
        cmd_pwm();
    } else if (!strcmp(argv[0], "TIMERCO") && argc == 3) {
        cmd_timerco(a[0], a[1]);
    } else if (!strcmp(argv[0], "DIGITAL")) {
        cmd_digital();
    } else if (!strcmp(argv[0], "LOAD")) {
        cmd_load();
    } else if (!strcmp(argv[0], "MELODY")) {
        cmd_melody();
    } else {
        reply("RESULT ERR unknown=%s", argv[0]);
    }
    reply("DONE");
}

void setup()
{
    Serial.begin(115200);
    reply("READY ToneAcceptance");
}

/**
 * @brief Collect one command line from the control port and run it.
 */
void loop()
{
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            line_buf[line_len] = '\0';
            dispatch(line_buf);
            line_len = 0;
        } else if (line_len + 1 < sizeof(line_buf)) {
            line_buf[line_len++] = (char)c;
        }
    }
}
