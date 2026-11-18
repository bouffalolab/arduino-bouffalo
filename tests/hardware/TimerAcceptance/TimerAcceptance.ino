/*
 * BLTimer hardware acceptance probe. Serial carries line commands from
 * tests/hardware/timer_acceptance.py at 115200 8N1; every command answers with one "RESULT ..."
 * line that the host script judges. Each callback reads the timer's own counter, which restarts
 * at every match, so it equals the callback latency in microseconds; micros() minus that latency
 * is the match time. Match-time gaps detect lost or duplicate callbacks exactly. micros() (MTIMER)
 * shares the crystal with TIMER0/1: the checks prove tick counts and latency, not crystal ppm.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <BLTimer.h>

#include "FreeRTOS.h"
#include "task.h"

#include "bflb_clock.h"
#include "bflb_timer.h"

static const size_t MAX_ARGS = 4;

/* Callback statistics; only the first limit callbacks are recorded, later ones count as extra. */
struct Stats {
    BLTimer *timer;
    uint32_t period;
    volatile uint32_t count;
    volatile uint32_t extra;
    volatile uint32_t limit;
    volatile uint32_t first_us;   /* micros() at the first callback. */
    volatile uint32_t last_us;
    volatile uint32_t first_match; /* Match times: micros() minus latency. */
    volatile uint32_t last_match;
    volatile uint32_t min_dt;      /* Callback-to-callback intervals. */
    volatile uint32_t max_dt;
    volatile uint32_t lat_first;
    volatile uint32_t lat_min;
    volatile uint32_t lat_max;
    volatile uint32_t lost;        /* Match gaps of more than one period. */
    volatile uint32_t dup;         /* Callbacks without a new match. */
};

static struct bflb_device_s *timer_devs[2];

/* State for calling BLTimer methods from inside its own callback. */
struct IsrCall {
    BLTimer *timer;
    volatile uint32_t count;
    volatile int stop_ok;
    volatile int start_ok;
    volatile int set_ok;
    volatile int begin_ok;
    volatile int index_after_end;
};

static char line_buf[160];
static size_t line_len;
static volatile uint32_t task_ticks;
static BLTimer load_timer;
static Stats load_stats;
static uint32_t load_start_us;
static uint32_t load_period;

/**
 * @brief Print one formatted line on the control port.
 */
static void reply(const char *fmt, ...)
{
    char out[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, sizeof(out), fmt, ap);
    va_end(ap);
    Serial.println(out);
}

/**
 * @brief Reset statistics for a timer and period; call while the timer is stopped or interrupts
 * are masked.
 */
static void reset_stats(Stats *s, BLTimer *timer, uint32_t period, uint32_t limit)
{
    s->timer = timer;
    s->period = period;
    s->count = 0;
    s->extra = 0;
    s->limit = limit;
    s->first_us = 0;
    s->last_us = 0;
    s->first_match = 0;
    s->last_match = 0;
    s->min_dt = 0xFFFFFFFFu;
    s->max_dt = 0;
    s->lat_first = 0;
    s->lat_min = 0xFFFFFFFFu;
    s->lat_max = 0;
    s->lost = 0;
    s->dup = 0;
}

/**
 * @brief Timer callback: record latency, match-time gaps and interval extremes.
 */
static void on_tick(void *arg)
{
    Stats *s = static_cast<Stats *>(arg);
    uint32_t lat = bflb_timer_get_countervalue(timer_devs[s->timer->timer()]);
    uint32_t now = micros();
    uint32_t match = now - lat;
    if (s->count >= s->limit) {
        s->extra = s->extra + 1;
        return;
    }
    if (s->count == 0) {
        s->first_us = now;
        s->first_match = match;
        s->lat_first = lat;
    } else {
        uint32_t dt = now - s->last_us;
        uint32_t gap = (match - s->last_match + s->period / 2) / s->period;
        if (dt < s->min_dt) {
            s->min_dt = dt;
        }
        if (dt > s->max_dt) {
            s->max_dt = dt;
        }
        if (gap == 0) {
            s->dup = s->dup + 1;
        } else if (gap > 1) {
            s->lost = s->lost + gap - 1;
        }
    }
    if (lat < s->lat_min) {
        s->lat_min = lat;
    }
    if (lat > s->lat_max) {
        s->lat_max = lat;
    }
    s->last_us = now;
    s->last_match = match;
    s->count = s->count + 1;
}

/**
 * @brief Return match-time drift: last minus first match minus (count - 1) periods.
 */
static long match_drift(const Stats *s)
{
    if (s->count < 2) {
        return 0;
    }
    return (long)(s->last_match - s->first_match) - (long)((uint64_t)s->period * (s->count - 1));
}

static volatile uint32_t plain_count;

/**
 * @brief Timer callback without an argument.
 */
static void on_plain(void)
{
    plain_count = plain_count + 1;
}

/**
 * @brief Increment a counter once per FreeRTOS tick to show the scheduler keeps running.
 */
static void counter_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(1);
        task_ticks = task_ticks + 1;
    }
}

/**
 * @brief Report clock identities used by the timer and the reference.
 */
static void cmd_info(void)
{
    reply("RESULT INFO xclk=%lu cpu=%lu tick_hz=%lu min=%lu max=%lu",
          (unsigned long)bflb_clk_get_system_clock(BFLB_SYSTEM_XCLK),
          (unsigned long)bflb_clk_get_system_clock(BFLB_SYSTEM_CPU_CLK), (unsigned long)configTICK_RATE_HZ,
          (unsigned long)BLTimer::MIN_PERIOD_US, (unsigned long)BLTimer::MAX_PERIOD_US);
}

/**
 * @brief Run a periodic timer for n callbacks and report count, first delay, drift and interval
 * extremes.
 */
static void cmd_rate(uint32_t period, uint32_t n)
{
    BLTimer t;
    Stats s;
    reset_stats(&s, &t, period, n);
    uint32_t t0 = micros();
    bool ok = t.begin(on_tick, &s, period);
    uint64_t timeout = (uint64_t)period * n * 12 / 10 + 200000;
    while (ok && s.count < n && (uint64_t)(micros() - t0) < timeout) {
        delay(1);
    }
    int idx = t.timer();
    t.end();
    reply("RESULT RATE period=%lu n=%lu ok=%d timer=%d count=%lu first_match=%lu drift=%ld lost=%lu dup=%lu "
          "lat_first=%lu lat_min=%lu lat_max=%lu min=%lu max=%lu",
          (unsigned long)period, (unsigned long)n, ok, idx, (unsigned long)s.count,
          (unsigned long)(s.first_match - t0), match_drift(&s), (unsigned long)s.lost, (unsigned long)s.dup,
          (unsigned long)s.lat_first, (unsigned long)s.lat_min, (unsigned long)s.lat_max,
          (unsigned long)s.min_dt, (unsigned long)s.max_dt);
}

/**
 * @brief Fire a one-shot timer, then re-arm it with start(), checking one callback each time.
 */
static void cmd_once(uint32_t delay_us)
{
    BLTimer t;
    Stats s;
    reset_stats(&s, &t, delay_us, 100);
    uint32_t wait_ms = delay_us * 3 / 1000 + 50;
    uint32_t t0 = micros();
    bool ok = t.beginOnce(on_tick, &s, delay_us);
    delay(wait_ms);
    uint32_t c1 = s.count;
    uint32_t lat1 = s.first_match - t0;
    int run1 = t.running();
    uint32_t t1 = micros();
    bool ok2 = t.start();
    int run_armed = t.running();
    delay(wait_ms);
    uint32_t c2 = s.count;
    uint32_t lat2 = s.last_match - t1;
    int run2 = t.running();
    int idx = t.timer();
    t.end();
    reply("RESULT ONCE delay=%lu ok=%d timer=%d count1=%lu lat1=%lu running1=%d restart=%d armed=%d count2=%lu "
          "lat2=%lu running2=%d",
          (unsigned long)delay_us, ok, idx, (unsigned long)c1, (unsigned long)lat1, run1, ok2, run_armed,
          (unsigned long)c2, (unsigned long)lat2, run2);
}

/**
 * @brief Exercise stop, start, setPeriod while running and stopped, end and re-acquire.
 */
static void cmd_life(void)
{
    BLTimer t;
    Stats s;
    reset_stats(&s, &t, 1000, 0xFFFFFFFFu);
    bool ok_begin = t.begin(on_tick, &s, 1000);
    delay(50);
    uint32_t run_count = s.count;

    bool ok_stop = t.stop();
    int run_after_stop = t.running();
    uint32_t c_stop = s.count;
    delay(50);
    uint32_t stopped_delta = s.count - c_stop;

    reset_stats(&s, &t, 1000, 0xFFFFFFFFu);
    uint32_t t0 = micros();
    bool ok_start = t.start();
    delay(30);
    uint32_t start_first = s.first_match - t0;
    uint32_t start_count = s.count;
    uint32_t start_min = s.min_dt;
    uint32_t start_max = s.max_dt;

    taskENTER_CRITICAL();
    reset_stats(&s, &t, 2000, 0xFFFFFFFFu);
    t0 = micros();
    bool ok_set = t.setPeriod(2000);
    taskEXIT_CRITICAL();
    delay(41);
    uint32_t set_first = s.first_match - t0;
    uint32_t set_count = s.count;
    uint32_t set_min = s.min_dt;
    uint32_t set_max = s.max_dt;

    t.stop();
    reset_stats(&s, &t, 500, 0xFFFFFFFFu);
    bool ok_set_stopped = t.setPeriod(500);
    int run_set_stopped = t.running();
    delay(20);
    uint32_t set_stopped_count = s.count;
    t0 = micros();
    t.start();
    delay(20);
    uint32_t restart_first = s.first_match - t0;
    uint32_t restart_count = s.count;
    uint32_t restart_min = s.min_dt;
    uint32_t restart_max = s.max_dt;

    t.end();
    reset_stats(&s, &t, 1000, 0xFFFFFFFFu);
    delay(20);
    uint32_t end_count = s.count;
    int end_index = t.timer();
    int end_running = t.running();
    int end_calls_ok = t.stop() || t.start() || t.setPeriod(1000);

    bool ok_again = t.begin(on_tick, &s, 1000);
    int again_index = t.timer();
    delay(10);
    uint32_t again_count = s.count;
    t.end();

    reply("RESULT LIFE begin=%d run_count=%lu stop=%d run_after_stop=%d stopped_delta=%lu start=%d start_first=%lu "
          "start_count=%lu start_min=%lu start_max=%lu set=%d set_first=%lu set_count=%lu set_min=%lu set_max=%lu",
          ok_begin, (unsigned long)run_count, ok_stop, run_after_stop, (unsigned long)stopped_delta, ok_start,
          (unsigned long)start_first, (unsigned long)start_count, (unsigned long)start_min,
          (unsigned long)start_max, ok_set, (unsigned long)set_first, (unsigned long)set_count,
          (unsigned long)set_min, (unsigned long)set_max);
    reply("RESULT LIFE2 set_stopped=%d run_set_stopped=%d set_stopped_count=%lu restart_first=%lu restart_count=%lu "
          "restart_min=%lu restart_max=%lu end_count=%lu end_index=%d end_running=%d end_calls_ok=%d again=%d "
          "again_index=%d again_count=%lu",
          ok_set_stopped, run_set_stopped, (unsigned long)set_stopped_count, (unsigned long)restart_first,
          (unsigned long)restart_count, (unsigned long)restart_min, (unsigned long)restart_max,
          (unsigned long)end_count, end_index, end_running, end_calls_ok, ok_again, again_index,
          (unsigned long)again_count);
}

/**
 * @brief Read the raw counter around stop/start to verify that start() clears it.
 */
static void cmd_clr(void)
{
    BLTimer t;
    Stats s;
    reset_stats(&s, &t, 1000000, 0xFFFFFFFFu);
    bool ok = t.begin(on_tick, &s, 1000000);
    struct bflb_device_s *dev = timer_devs[t.timer() == 1 ? 1 : 0];
    delay(300);
    uint32_t running = bflb_timer_get_countervalue(dev);
    t.stop();
    uint32_t stopped = bflb_timer_get_countervalue(dev);
    /* Diagnostic: re-enable with raw lhal, bypassing BLTimer's counter clear. */
    bflb_timer_start(dev);
    uint32_t raw_start = bflb_timer_get_countervalue(dev);
    bflb_timer_stop(dev);
    t.start();
    uint32_t after_start = bflb_timer_get_countervalue(dev);
    delay(100);
    uint32_t after_100ms = bflb_timer_get_countervalue(dev);
    t.end();
    reply("RESULT CLR ok=%d running=%lu stopped=%lu raw_start=%lu after_start=%lu after_100ms=%lu", ok,
          (unsigned long)running, (unsigned long)stopped, (unsigned long)raw_start, (unsigned long)after_start,
          (unsigned long)after_100ms);
}

/**
 * @brief Run two timers together, check a third claim fails, then reuse a released timer.
 */
static void cmd_dual(uint32_t p0, uint32_t p1, uint32_t ms)
{
    BLTimer a, b, c;
    Stats sa, sb, sc;
    reset_stats(&sa, &a, p0, 0xFFFFFFFFu);
    reset_stats(&sb, &b, p1, 0xFFFFFFFFu);
    reset_stats(&sc, &c, 1000, 0xFFFFFFFFu);
    uint32_t ta0 = micros();
    bool ok_a = a.begin(on_tick, &sa, p0);
    uint32_t tb0 = micros();
    bool ok_b = b.begin(on_tick, &sb, p1);
    bool ok_c = c.begin(on_tick, &sc, 1000);
    int ia = a.timer(), ib = b.timer(), ic = c.timer();
    delay(ms);
    a.end();
    uint32_t window_a = micros() - ta0;
    b.end();
    uint32_t window_b = micros() - tb0;
    bool ok_c2 = c.begin(on_tick, &sc, 1000);
    int ic2 = c.timer();
    c.end();
    reply("RESULT DUAL p0=%lu p1=%lu a=%d b=%d c=%d ia=%d ib=%d ic=%d count_a=%lu window_a=%lu count_b=%lu "
          "window_b=%lu drift_a=%ld drift_b=%ld lost=%lu dup=%lu lat_max_a=%lu lat_max_b=%lu c_count=%lu c2=%d ic2=%d",
          (unsigned long)p0, (unsigned long)p1, ok_a, ok_b, ok_c, ia, ib, ic, (unsigned long)sa.count,
          (unsigned long)window_a, (unsigned long)sb.count, (unsigned long)window_b, match_drift(&sa),
          match_drift(&sb), (unsigned long)(sa.lost + sb.lost), (unsigned long)(sa.dup + sb.dup),
          (unsigned long)sa.lat_max, (unsigned long)sb.lat_max, (unsigned long)sc.count, ok_c2, ic2);
}

/**
 * @brief Check out-of-range periods, null callbacks and calls without a held timer are rejected
 * and leave state unchanged; check the maximum period and the plain callback form.
 */
static void cmd_reject(void)
{
    BLTimer t;
    Stats s;
    reset_stats(&s, &t, 10, 0xFFFFFFFFu);
    int r0 = t.begin(on_tick, &s, 0);
    int r9 = t.begin(on_tick, &s, 9);
    int rnull = t.begin(static_cast<void (*)(void *)>(nullptr), nullptr, 1000);
    int rnull_plain = t.begin(static_cast<void (*)()>(nullptr), 1000);
    int ronce9 = t.beginOnce(on_tick, &s, 9);
    int idx_after = t.timer();
    int unheld = t.setPeriod(1000) || t.start() || t.stop();

    int r10 = t.begin(on_tick, &s, 10);
    delay(5);
    int set9 = t.setPeriod(9);
    taskENTER_CRITICAL();
    reset_stats(&s, &t, 10, 0xFFFFFFFFu);
    taskEXIT_CRITICAL();
    delay(10);
    uint32_t n = s.count;
    uint32_t mean10_x100 = n > 1 ? (uint32_t)((uint64_t)(s.last_match - s.first_match) * 100 / (n - 1)) : 0;
    int run10 = t.running();
    int set_max = t.setPeriod(BLTimer::MAX_PERIOD_US);
    int run_max = t.running();
    t.end();
    int begin_max = t.begin(on_tick, &s, BLTimer::MAX_PERIOD_US);
    t.end();

    plain_count = 0;
    int plain = t.begin(on_plain, 1000);
    delay(20);
    uint32_t plain_n = plain_count;
    t.end();
    reply("RESULT REJECT r0=%d r9=%d rnull=%d rnull_plain=%d ronce9=%d idx_after=%d unheld=%d r10=%d set9=%d "
          "run10=%d mean10_x100=%lu set_max=%d run_max=%d begin_max=%d plain=%d plain_count=%lu",
          r0, r9, rnull, rnull_plain, ronce9, idx_after, unheld, r10, set9, run10, (unsigned long)mean10_x100,
          set_max, run_max, begin_max, plain, (unsigned long)plain_n);
}

/**
 * @brief Callback that tries every BLTimer method on its own timer once.
 */
static void on_isr_call(void *arg)
{
    IsrCall *c = static_cast<IsrCall *>(arg);
    if (c->count == 0) {
        c->stop_ok = c->timer->stop();
        c->start_ok = c->timer->start();
        c->set_ok = c->timer->setPeriod(100);
        c->begin_ok = c->timer->begin(on_isr_call, arg, 100);
        c->timer->end();
        c->index_after_end = c->timer->timer();
    }
    c->count = c->count + 1;
}

/**
 * @brief Check that methods called inside a callback fail and leave the timer running.
 */
static void cmd_isrcall(void)
{
    BLTimer t;
    IsrCall c = { &t, 0, -1, -1, -1, -1, -2 };
    int idx_before = -1;
    bool ok = t.begin(on_isr_call, &c, 1000);
    idx_before = t.timer();
    delay(20);
    uint32_t n = c.count;
    int running = t.running();
    t.end();
    reply("RESULT ISRCALL ok=%d count=%lu stop=%d start=%d set=%d begin=%d idx_before=%d idx_after_end=%d running=%d",
          ok, (unsigned long)n, c.stop_ok, c.start_ok, c.set_ok, c.begin_ok, idx_before, c.index_after_end,
          running);
}

/**
 * @brief Start (period > 0) or stop (period 0) a background timer that stays active across commands.
 */
static void cmd_load(uint32_t period)
{
    if (period > 0) {
        reset_stats(&load_stats, &load_timer, period, 0xFFFFFFFFu);
        load_period = period;
        load_start_us = micros();
        bool ok = load_timer.begin(on_tick, &load_stats, period);
        reply("RESULT LOAD period=%lu ok=%d timer=%d", (unsigned long)period, ok, load_timer.timer());
        return;
    }
    load_timer.end();
    uint32_t elapsed = micros() - load_start_us;
    reply("RESULT UNLOAD period=%lu count=%lu elapsed=%lu drift=%ld lost=%lu dup=%lu lat_max=%lu min=%lu max=%lu",
          (unsigned long)load_period, (unsigned long)load_stats.count, (unsigned long)elapsed,
          match_drift(&load_stats), (unsigned long)load_stats.lost, (unsigned long)load_stats.dup,
          (unsigned long)load_stats.lat_max, (unsigned long)load_stats.min_dt, (unsigned long)load_stats.max_dt);
}

/**
 * @brief Measure delay(ms), the FreeRTOS counter task and a busy loop in one window.
 */
static void cmd_sys(uint32_t ms)
{
    uint32_t ticks0 = task_ticks;
    uint32_t t0 = micros();
    delay(ms);
    uint32_t delay_us = micros() - t0;
    uint32_t ticks = task_ticks - ticks0;
    uint32_t loops = 0;
    t0 = micros();
    while (micros() - t0 < 100000) {
        loops++;
    }
    reply("RESULT SYS ms=%lu delay_us=%lu task_ticks=%lu spin_loops_100ms=%lu load_count=%lu", (unsigned long)ms,
          (unsigned long)delay_us, (unsigned long)ticks, (unsigned long)loops, (unsigned long)load_stats.count);
}

/**
 * @brief Split a command line into whitespace-separated arguments and run the command.
 */
static void dispatch(char *line)
{
    if (strncmp(line, "ECHO ", 5) == 0) {
        reply("RESULT ECHO %s", line + 5);
        return;
    }
    char *argv[MAX_ARGS] = {};
    size_t argc = 0;
    for (char *tok = strtok(line, " "); tok != nullptr && argc < MAX_ARGS; tok = strtok(nullptr, " ")) {
        argv[argc++] = tok;
    }
    if (argc == 0) {
        return;
    }
    uint32_t a1 = argc > 1 ? strtoul(argv[1], nullptr, 0) : 0;
    uint32_t a2 = argc > 2 ? strtoul(argv[2], nullptr, 0) : 0;
    uint32_t a3 = argc > 3 ? strtoul(argv[3], nullptr, 0) : 0;
    if (strcmp(argv[0], "INFO") == 0) {
        cmd_info();
    } else if (strcmp(argv[0], "RATE") == 0) {
        cmd_rate(a1, a2);
    } else if (strcmp(argv[0], "ONCE") == 0) {
        cmd_once(a1);
    } else if (strcmp(argv[0], "LIFE") == 0) {
        cmd_life();
    } else if (strcmp(argv[0], "CLR") == 0) {
        cmd_clr();
    } else if (strcmp(argv[0], "DUAL") == 0) {
        cmd_dual(a1, a2, a3);
    } else if (strcmp(argv[0], "REJECT") == 0) {
        cmd_reject();
    } else if (strcmp(argv[0], "ISRCALL") == 0) {
        cmd_isrcall();
    } else if (strcmp(argv[0], "LOAD") == 0) {
        cmd_load(a1);
    } else if (strcmp(argv[0], "SYS") == 0) {
        cmd_sys(a1);
    } else {
        reply("RESULT ERROR unknown=%s", argv[0]);
    }
}

void setup()
{
    timer_devs[0] = bflb_device_get_by_name("timer0");
    timer_devs[1] = bflb_device_get_by_name("timer1");
    Serial.begin(115200);
    xTaskCreate(counter_task, "ticks", 1024 / sizeof(StackType_t), nullptr, 1, nullptr);
    reply("READY TimerAcceptance");
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
