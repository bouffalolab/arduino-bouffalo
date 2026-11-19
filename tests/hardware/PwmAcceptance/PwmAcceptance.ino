/*
 * analogWrite hardware acceptance probe for Ai-M64P-32S-Kit. It needs the board's jumpers
 * GPIO8-GPIO22, GPIO14-GPIO15, GPIO18-GPIO19 and GPIO20-GPIO23. Serial carries line commands from
 * tests/hardware/pwm_acceptance.py at 115200 8N1; every command answers with "RESULT ..." lines
 * that the host script judges.
 *
 * A pin in PWM function reads 0 on its own input bit, so each output is sampled on its jumpered
 * partner: the probe polls the GPIO input register with interrupts off and timestamps every edge
 * with the CPU cycle counter. The CPU clock and the 40 MHz PWM clock come from the same crystal,
 * so frequency checks confirm the PWM configuration, not the crystal. The ADC pins GPIO0-7/12/13
 * stay in PWM function while the GPADC samples them through LHAL, which confirms the GPIO % 4
 * channel map on pins without a jumper, including channel 1. analogWriteFrequency() commands leave
 * the frequency at its 1 kHz default when they finish.
 */
#include <initializer_list>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <BLTimer.h>

#include "bflb_adc_v2.h"
#include "bflb_clock.h"
#include "bflb_irq.h"
#include "bflb_pwm_v2.h"
#include "hardware/pwm_v2_reg.h"

static const uint32_t GLB_BASE_ADDR = 0x20000000;
static const uint32_t GPIO_CFG0 = GLB_BASE_ADDR + 0x8C4; /* One 32-bit config word per GPIO. */
static const uint32_t GPIO_IN = GLB_BASE_ADDR + 0xAC4;   /* Input levels of GPIO0-31. */
static const uint32_t PWM_IO_SEL = GLB_BASE_ADDR + 0x1D0;
static const uint32_t FUNC_SWGPIO = 11;
static const uint32_t FUNC_PWM = 16;

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

static uint32_t gpio_func(uint8_t gpio)
{
    return (reg(GPIO_CFG0 + 4 * gpio) >> 8) & 0x1f;
}

static uint32_t gpio_oe(uint8_t gpio)
{
    return (reg(GPIO_CFG0 + 4 * gpio) >> 6) & 1;
}

/* Return every GPIO0-31 to a floating input, which also frees every PWM channel. */
static void release_all(void)
{
    for (uint8_t p = 0; p < 32; p++) {
        pinMode(p, INPUT);
    }
}

struct Edge {
    uint32_t t;
    uint32_t v;
};
static Edge edges[600];

/* Edge statistics of one input over a capture window, in CPU cycles. */
struct Wave {
    uint32_t rises, falls, lvl0, lvl1, first_rise;
    uint64_t per_sum, hi_sum;
    uint32_t per_n, hi_n, per_min, per_max, hi_min, hi_max;
};

/* Declared here so the Arduino prototype generator does not place them before Wave. */
static Wave analyze(uint8_t pin, uint32_t n, uint32_t v0, uint32_t vend);
static void report(const char *tag, uint8_t pin, const Wave &w, uint32_t iters, uint32_t win);

/**
 * @brief Poll GPIO_IN with interrupts off for `ms`; record each change of the masked bits.
 */
static uint32_t capture(uint32_t mask, uint32_t ms, uint32_t &n, uint32_t &iters, uint32_t &win, uint32_t &v0)
{
    win = bflb_clk_get_system_clock(BFLB_SYSTEM_CPU_CLK) / 1000 * ms;
    uintptr_t flags = bflb_irq_save();
    uint32_t t0 = cycles();
    uint32_t prev = reg(GPIO_IN) & mask;
    uint32_t it = 0, k = 0, t;
    v0 = prev;
    do {
        uint32_t v = reg(GPIO_IN) & mask;
        t = cycles();
        it++;
        if (v != prev) {
            if (k < sizeof(edges) / sizeof(edges[0])) {
                edges[k].t = t - t0;
                edges[k].v = v;
                k++;
            }
            prev = v;
        }
    } while (t - t0 < win);
    bflb_irq_restore(flags);
    n = k;
    iters = it;
    return prev;
}

static Wave analyze(uint8_t pin, uint32_t n, uint32_t v0, uint32_t vend)
{
    Wave w = {};
    w.per_min = w.hi_min = UINT32_MAX;
    w.lvl0 = (v0 >> pin) & 1;
    w.lvl1 = (vend >> pin) & 1;
    uint32_t last = w.lvl0, t_rise = 0;
    bool have_rise = false;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t lv = (edges[i].v >> pin) & 1;
        if (lv == last) {
            continue;
        }
        last = lv;
        if (lv) {
            if (have_rise) {
                uint32_t p = edges[i].t - t_rise;
                w.per_sum += p;
                w.per_n++;
                w.per_min = p < w.per_min ? p : w.per_min;
                w.per_max = p > w.per_max ? p : w.per_max;
            } else {
                w.first_rise = edges[i].t;
            }
            w.rises++;
            have_rise = true;
            t_rise = edges[i].t;
        } else {
            w.falls++;
            if (have_rise) {
                uint32_t h = edges[i].t - t_rise;
                w.hi_sum += h;
                w.hi_n++;
                w.hi_min = h < w.hi_min ? h : w.hi_min;
                w.hi_max = h > w.hi_max ? h : w.hi_max;
            }
        }
    }
    if (w.per_n == 0) {
        w.per_min = 0;
    }
    if (w.hi_n == 0) {
        w.hi_min = 0;
    }
    return w;
}

static void report(const char *tag, uint8_t pin, const Wave &w, uint32_t iters, uint32_t win)
{
    reply("RESULT %s in=%u rises=%lu falls=%lu lvl0=%lu lvl1=%lu per=%lu per_min=%lu per_max=%lu hi=%lu hi_min=%lu "
          "hi_max=%lu first_rise=%lu iters=%lu win=%lu cpu=%lu per_sum=%lu per_n=%lu hi_sum=%lu hi_n=%lu",
          tag, pin, w.rises, w.falls, w.lvl0, w.lvl1, w.per_n ? (uint32_t)(w.per_sum / w.per_n) : 0, w.per_min,
          w.per_max, w.hi_n ? (uint32_t)(w.hi_sum / w.hi_n) : 0, w.hi_min, w.hi_max, w.first_rise, iters, win,
          bflb_clk_get_system_clock(BFLB_SYSTEM_CPU_CLK), (uint32_t)w.per_sum, w.per_n, (uint32_t)w.hi_sum, w.hi_n);
}

/**
 * @brief Capture up to three inputs together for `ms` and report each. Sums stay within 32 bits
 * for windows up to 13 s.
 */
static void measure_for(uint32_t ms, const char *tag, int a, int b = -1, int c = -1)
{
    uint32_t mask = 0;
    for (int p : {a, b, c}) {
        if (p >= 0) {
            mask |= 1u << p;
        }
    }
    uint32_t n, iters, win, v0;
    uint32_t vend = capture(mask, ms, n, iters, win, v0);
    for (int p : {a, b, c}) {
        if (p >= 0) {
            report(tag, p, analyze(p, n, v0, vend), iters, win);
        }
    }
}

/* 25 ms is 25 periods at the default 1 kHz. */
static void measure(const char *tag, int a, int b = -1, int c = -1)
{
    measure_for(25, tag, a, b, c);
}

/* Report the running PWM0 counter: divider, period count and LHAL's rounded frequency. */
static void report_counter(const char *tag)
{
    struct bflb_device_s *pwm = bflb_device_get_by_name("pwm_v2_0");
    reply("RESULT %s div=%lu period=%lu freq=%lu func23=%lu", tag,
          reg(pwm->reg_base + PWM_MC0_CONFIG0_OFFSET) & 0xffff, reg(pwm->reg_base + PWM_MC0_PERIOD_OFFSET) & 0xffff,
          bflb_pwm_v2_get_frequency(pwm), gpio_func(23));
}

/* INFO: clocks, routing register and the variant's PWM pin declaration for pins 0-40. */
static void cmd_info(void)
{
    uint64_t has = 0;
    for (uint32_t p = 0; p <= 40; p++) {
        has |= (uint64_t)(digitalPinHasPWM(p) ? 1 : 0) << p;
    }
    reply("RESULT INFO cpu=%lu xclk=%lu io_sel=%lu num_digital=%d has_lo=%lu has_hi=%lu", bflb_clk_get_system_clock(BFLB_SYSTEM_CPU_CLK),
          bflb_clk_get_system_clock(BFLB_SYSTEM_XCLK), reg(PWM_IO_SEL) & 1, NUM_DIGITAL_PINS, (uint32_t)has,
          (uint32_t)(has >> 32));
}

/* BOOT: the first analogWrite after reset, at the default resolution. */
static void cmd_boot(void)
{
    release_all();
    analogWrite(23, 128);
    delay(3);
    struct bflb_device_s *pwm = bflb_device_get_by_name("pwm_v2_0");
    reply("RESULT BOOTCFG io_sel=%lu freq=%lu func23=%lu", reg(PWM_IO_SEL) & 1, bflb_pwm_v2_get_frequency(pwm),
          gpio_func(23));
    measure("BOOT", 20);
    release_all();
}

/* SHORTS: each jumper passes both levels in both directions against the opposite pull. */
static void cmd_shorts(void)
{
    static const uint8_t pairs[4][2] = {{8, 22}, {14, 15}, {18, 19}, {20, 23}};
    for (auto &pr : pairs) {
        uint32_t ok = 0;
        for (int dir = 0; dir < 2; dir++) {
            uint8_t a = pr[dir], b = pr[1 - dir];
            release_all();
            pinMode(b, INPUT_PULLDOWN);
            pinMode(a, OUTPUT);
            digitalWrite(a, HIGH);
            delay(1);
            ok += digitalRead(b) == HIGH;
            pinMode(b, INPUT_PULLUP);
            digitalWrite(a, LOW);
            delay(1);
            ok += digitalRead(b) == LOW;
        }
        reply("RESULT SHORT a=%u b=%u ok=%lu", pr[0], pr[1], ok);
    }
    release_all();
}

/* DUTY out in bits value: one output at the given resolution, sampled on its partner. */
static void cmd_duty(int out, int in, int bits, long value)
{
    release_all();
    analogWriteResolution(bits);
    analogWrite(out, (int)value);
    delay(3);
    measure("DUTY", in);
    release_all();
}

/* RESINV bad: set 12 bits, then an invalid width, then write 2048 (50% at 12 bits) on GPIO23. */
static void cmd_resinv(int bad)
{
    release_all();
    analogWriteResolution(12);
    analogWriteResolution(bad);
    analogWrite(23, 2048);
    delay(3);
    measure("RESINV", 20);
    release_all();
}

/* MULTI a b c: GPIO23 (CH3), GPIO8 (CH0), GPIO14 (CH2) and GPIO1 (CH1, 128) at 8 bits; no release. */
static void cmd_multi(int a, int b, int c)
{
    analogWriteResolution(8);
    analogWrite(23, a);
    analogWrite(8, b);
    analogWrite(14, c);
    analogWrite(1, 128);
    delay(3);
    measure("MULTI", 20, 22, 15);
}

/* ADCMAP p: channel p % 4 driven by p, the other channels by GPIO24-27 at distinct duties; the
 * GPADC samples p's pad 1000 times and counts readings above half scale. */
static void cmd_adcmap(int p)
{
    static const int duty[4] = {26, 77, 153, 217}; /* About 10, 30, 60 and 85 % at 8 bits. */
    release_all();
    analogRead(p); /* Initializes the ADC and selects p's channel. */
    analogWriteResolution(8);
    for (int ch = 0; ch < 4; ch++) {
        analogWrite(ch == p % 4 ? p : 24 + ch, duty[ch]);
    }
    delay(3);
    struct bflb_device_s *adc = bflb_device_get_by_name("adc_v2_0");
    uint32_t n = 1000, high = 0;
    for (uint32_t i = 0; i < n; i++) {
        bflb_adc_clear_fifo(adc);
        bflb_adc_start_conversion(adc);
        while (bflb_adc_get_count(adc) == 0) {
        }
        uint32_t raw = bflb_adc_read_raw(adc);
        bflb_adc_stop_conversion(adc);
        struct bflb_adc_result_s r;
        bflb_adc_parse_result(adc, &raw, &r, 1);
        high += (uint32_t)r.value >= 32768;
    }
    reply("RESULT ADCMAP pin=%d func=%lu n=%lu high=%lu", p, gpio_func(p), n, high);
    release_all();
}

/* CONFLICT: GPIO8 holds channel 0; GPIO20 (also channel 0) must be refused until GPIO8 lets go. */
static void cmd_conflict(void)
{
    release_all();
    analogWriteResolution(8);
    analogWrite(8, 128);
    pinMode(20, OUTPUT);
    digitalWrite(20, LOW);
    uint32_t before = reg(GPIO_CFG0 + 4 * 20);
    analogWrite(20, 64);
    uint32_t after = reg(GPIO_CFG0 + 4 * 20);
    delay(3);
    reply("RESULT CONFLICTCFG same=%d func20=%lu", before == after, gpio_func(20));
    measure("CONFLICT_REFUSED", 23, 22);
    digitalWrite(20, HIGH);
    delay(1);
    measure("CONFLICT_DIGITAL", 23);
    pinMode(8, INPUT);
    analogWrite(20, 64);
    delay(3);
    measure("CONFLICT_FREED", 23);
    release_all();
}

/* INVALID: requests on reserved, console and out-of-range pins change neither pins nor PWM. */
static void cmd_invalid(void)
{
    static const int pins[] = {32, 33, 34, 35, 36, 37, 40, 100, 255};
    struct bflb_device_s *pwm = bflb_device_get_by_name("pwm_v2_0");
    release_all();
    analogWriteResolution(8);
    analogWrite(23, 64); /* A running channel whose state must stay as it is. */
    delay(3);
    uint32_t snap[2][10];
    for (int k = 0; k < 2; k++) {
        for (int g = 32; g <= 36; g++) {
            snap[k][g - 32] = reg(GPIO_CFG0 + 4 * g);
        }
        snap[k][5] = reg(pwm->reg_base + PWM_MC0_CONFIG1_OFFSET);
        for (int ch = 0; ch < 4; ch++) {
            snap[k][6 + ch] = reg(pwm->reg_base + PWM_MC0_CH0_THRE_OFFSET + 4 * ch);
        }
        if (k == 0) {
            for (int p : pins) {
                analogWrite(p, 128);
            }
        }
    }
    uint32_t changed = 0;
    for (int i = 0; i < 10; i++) {
        changed |= (snap[0][i] != snap[1][i]) << i;
    }
    reply("RESULT INVALID checked=%u changed=0x%lx", (unsigned)(sizeof(pins) / sizeof(pins[0])), changed);
    measure("INVALID_RUNNING", 20);
    release_all();
}

/* BACK: leaving PWM through digitalWrite, pinMode and analogRead. */
static void cmd_back(void)
{
    release_all();
    analogWriteResolution(8);
    analogWrite(23, 128);
    delay(3);
    digitalWrite(23, HIGH);
    reply("RESULT BACKCFG_DW func=%lu oe=%lu", gpio_func(23), gpio_oe(23));
    measure("BACK_DW_HIGH", 20);
    digitalWrite(23, LOW);
    measure("BACK_DW_LOW", 20);

    analogWrite(23, 128);
    delay(3);
    pinMode(23, INPUT);
    reply("RESULT BACKCFG_PINMODE func=%lu oe=%lu", gpio_func(23), gpio_oe(23));
    pinMode(20, OUTPUT);
    digitalWrite(20, HIGH);
    delay(1);
    int follow_hi = digitalRead(23);
    digitalWrite(20, LOW);
    delay(1);
    int follow_lo = digitalRead(23);
    reply("RESULT BACKINPUT follow_hi=%d follow_lo=%d", follow_hi, follow_lo);
    pinMode(20, INPUT);

    analogWrite(19, 128); /* Channel 3 was freed by pinMode(23). */
    delay(3);
    measure("BACK_CH3_FREE", 18);
    pinMode(19, INPUT);

    analogWrite(4, 128);
    delay(3);
    int v = analogRead(A4);
    analogWrite(8, 64); /* Channel 0 was freed by analogRead(A4). */
    delay(3);
    reply("RESULT BACKADC value=%d func4=%lu", v, gpio_func(4));
    measure("BACK_CH0_FREE", 22);
    release_all();
}

/* FREQ hz bits value ms: set the frequency, then write GPIO23 and sample it on GPIO20 for ms; then
 * time the return to 1 kHz, which waits for the running period to end. */
static void cmd_freq(uint32_t hz, int bits, long value, uint32_t ms)
{
    release_all();
    analogWriteFrequency(hz);
    analogWriteResolution(bits);
    analogWrite(23, (int)value);
    delay(3);
    report_counter("FREQCFG");
    measure_for(ms, "FREQ", 20);
    release_all();
    uint32_t t0 = micros();
    analogWriteFrequency(1000);
    reply("RESULT FREQBACK us=%lu", micros() - t0);
    report_counter("FREQBACKCFG");
}

/* FMULTI hz ms: three channels start at 1 kHz, then the frequency changes under them and back. */
static void cmd_fmulti(uint32_t hz, uint32_t ms)
{
    release_all();
    analogWriteResolution(8);
    analogWrite(23, 64);
    analogWrite(8, 191);
    analogWrite(14, 128);
    delay(3);
    analogWriteFrequency(hz);
    delay(3);
    report_counter("FMULTICFG");
    measure_for(ms, "FMULTI", 20, 22, 15);
    analogWriteFrequency(1000);
    delay(3);
    report_counter("FMULTIBACKCFG");
    measure("FMULTI_BACK", 20, 22, 15);
    release_all();
}

/* FREQINV bad: GPIO23 runs at 20 kHz; an invalid frequency must leave the counter and duty as they are. */
static void cmd_freqinv(uint32_t bad)
{
    struct bflb_device_s *pwm = bflb_device_get_by_name("pwm_v2_0");
    static const uint32_t offs[] = {PWM_MC0_CONFIG0_OFFSET, PWM_MC0_PERIOD_OFFSET, PWM_MC0_CONFIG1_OFFSET,
                                    PWM_MC0_CH0_THRE_OFFSET + 4 * 3};
    uint32_t before[4], changed = 0;
    release_all();
    analogWriteFrequency(20000);
    analogWriteResolution(8);
    analogWrite(23, 128);
    delay(3);
    for (int i = 0; i < 4; i++) {
        before[i] = reg(pwm->reg_base + offs[i]);
    }
    analogWriteFrequency(bad);
    for (int i = 0; i < 4; i++) {
        changed |= (reg(pwm->reg_base + offs[i]) != before[i]) << i;
    }
    reply("RESULT FREQINV bad=%lu changed=0x%lx", bad, changed);
    measure_for(5, "FREQINV_RUNNING", 20);
    release_all();
    analogWriteFrequency(1000);
}

/* FREQFIRST: right after reset, set 20 kHz before any analogWrite, then write GPIO23. */
static void cmd_freqfirst(void)
{
    pinMode(20, INPUT); /* The sampling input; leaves PWM untouched. */
    analogWriteFrequency(20000);
    analogWrite(23, 128);
    delay(3);
    report_counter("FREQFIRSTCFG");
    measure_for(5, "FREQFIRST", 20);
    release_all();
    analogWriteFrequency(1000);
}

static volatile uint32_t ticks;

static void on_tick()
{
    ticks++;
}

/* TIMER: BLTimer at 100 us keeps its rate while three PWM channels run, and PWM keeps its duty. */
static void cmd_timer(void)
{
    static BLTimer timer;
    release_all();
    bool began = timer.begin(on_tick, 100);
    analogWriteResolution(8);
    analogWrite(23, 64);
    analogWrite(8, 191);
    analogWrite(14, 128);
    delay(3);
    measure("TIMER_PWM", 20, 22, 15);
    uint32_t t0 = ticks;
    uint32_t m0 = micros();
    delay(200);
    uint32_t dt = ticks - t0, dus = micros() - m0;
    timer.end();
    reply("RESULT TIMER began=%d idx_free=%d ticks=%lu us=%lu", began, timer.timer() == -1, dt, dus);
    release_all();
}

static void dispatch(char *line)
{
    char *argv[6];
    int argc = 0;
    for (char *tok = strtok(line, " "); tok && argc < 6; tok = strtok(nullptr, " ")) {
        argv[argc++] = tok;
    }
    if (argc == 0) {
        return;
    }
    long a[5] = {};
    for (int i = 1; i < argc; i++) {
        a[i - 1] = (long)strtoul(argv[i], nullptr, 0); /* Negative values wrap back through long. */
    }
    if (!strcmp(argv[0], "INFO")) {
        cmd_info();
    } else if (!strcmp(argv[0], "BOOT")) {
        cmd_boot();
    } else if (!strcmp(argv[0], "SHORTS")) {
        cmd_shorts();
    } else if (!strcmp(argv[0], "DUTY") && argc == 5) {
        cmd_duty(a[0], a[1], a[2], a[3]);
    } else if (!strcmp(argv[0], "RESINV") && argc == 2) {
        cmd_resinv(a[0]);
    } else if (!strcmp(argv[0], "MULTI") && argc == 4) {
        cmd_multi(a[0], a[1], a[2]);
    } else if (!strcmp(argv[0], "RELEASE")) {
        release_all();
        reply("RESULT RELEASE");
    } else if (!strcmp(argv[0], "ADCMAP") && argc == 2) {
        cmd_adcmap(a[0]);
    } else if (!strcmp(argv[0], "CONFLICT")) {
        cmd_conflict();
    } else if (!strcmp(argv[0], "INVALID")) {
        cmd_invalid();
    } else if (!strcmp(argv[0], "BACK")) {
        cmd_back();
    } else if (!strcmp(argv[0], "TIMER")) {
        cmd_timer();
    } else if (!strcmp(argv[0], "FREQ") && argc == 5) {
        cmd_freq(a[0], a[1], a[2], a[3]);
    } else if (!strcmp(argv[0], "FMULTI") && argc == 3) {
        cmd_fmulti(a[0], a[1]);
    } else if (!strcmp(argv[0], "FREQINV") && argc == 2) {
        cmd_freqinv(a[0]);
    } else if (!strcmp(argv[0], "FREQFIRST")) {
        cmd_freqfirst();
    } else {
        reply("RESULT ERR unknown=%s", argv[0]);
    }
    reply("DONE");
}

void setup()
{
    Serial.begin(115200);
    reply("READY PwmAcceptance");
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
