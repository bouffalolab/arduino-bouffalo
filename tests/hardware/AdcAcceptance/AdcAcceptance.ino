/*
 * analogRead hardware acceptance probe for Ai-M64P-32S-Kit without external wiring. Serial
 * carries line commands from tests/hardware/adc_acceptance.py at 115200 8N1; every command
 * answers with one "RESULT ..." line that the host script judges.
 *
 * Without wiring there is no known external voltage. The probe therefore uses what the board
 * itself provides: a pin driven as an output and then switched to analog input by analogRead()
 * keeps part of its charge, which tells the channel mapping apart; GPIO2/GPIO3 (A2/A3) sit at a
 * steady level near 40 mV on this board, which serves resolution and noise checks. RAILS and the
 * VBAT/2 readings go through LHAL directly and are exploratory only.
 */
#include <initializer_list>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <WiFi.h>

#include "FreeRTOS.h"
#include "task.h"

#include "bflb_adc_v2.h"
#include "bflb_gpio.h"

static const uint8_t APINS[] = {A0, A1, A2, A3, A4, A5, A6, A7, A8, A9};
static const uint8_t ACHANNELS[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}; /* Datasheet channel per A pin. */
static const size_t NA = sizeof(APINS);

static char line_buf[160];
static size_t line_len;

/* Values captured in setup() before anything else touches the ADC. */
static int boot_value;
static uint32_t boot_us;

static volatile bool scan_run;
static volatile bool scan_done;
static volatile uint32_t scan_count;
static volatile int scan_found;

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

/* Running statistics of integer samples. */
struct Stats {
    uint32_t n = 0;
    int64_t sum = 0;
    int64_t sum2 = 0;
    int32_t lo = INT32_MAX;
    int32_t hi = INT32_MIN;

    void add(int32_t v)
    {
        n++;
        sum += v;
        sum2 += (int64_t)v * v;
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
    }
    long mean_x100() const
    {
        return n ? (long)(sum * 100 / (int64_t)n) : 0;
    }
    long sd_x100() const
    {
        if (n == 0) {
            return 0;
        }
        double m = (double)sum / n;
        double var = (double)sum2 / n - m * m;
        return (long)(sqrt(var > 0 ? var : 0) * 100);
    }
};

/* Declared here so the Arduino prototype generator does not place it before Stats. */
static void sample(uint32_t n, Stats &a2, Stats &a3, Stats &vbat, uint32_t &fail);

/**
 * @brief Drive every A pin as an output: high on index `high`, low on the others.
 */
static void drive_all(int high)
{
    for (size_t i = 0; i < NA; i++) {
        pinMode(APINS[i], OUTPUT);
        digitalWrite(APINS[i], (int)i == high ? HIGH : LOW);
    }
}

/**
 * @brief Report first-call and steady-state analogRead cost and the boot-default reading.
 */
static void cmd_info(void)
{
    Stats t;
    analogReadResolution(10);
    for (int i = 0; i < 100; i++) {
        uint32_t t0 = micros();
        analogRead(A2);
        t.add((int32_t)(micros() - t0));
    }
    reply("RESULT INFO num=%d boot_value=%d boot_us=%lu call_us_min=%ld call_us_max=%ld call_us_mean_x100=%ld",
          NUM_ANALOG_INPUTS, boot_value, (unsigned long)boot_us, (long)t.lo, (long)t.hi, t.mean_x100());
}

/**
 * @brief Drive A[x] high and the other A pins low, then read every A pin at 10 bits. A wrong
 * channel mapping reads a driven pin (top code); the right one reads only A[x] above its peers.
 */
static void cmd_map(uint32_t x)
{
    char text[160];
    size_t len = 0;

    analogReadResolution(10);
    drive_all((int)x);
    delay(1);
    for (size_t y = 0; y < NA; y++) {
        int v = analogRead(APINS[y]);
        len += snprintf(text + len, sizeof(text) - len, " v%u=%d", (unsigned)y, v);
        pinMode(APINS[y], OUTPUT); /* Restore this pin's drive before the next read. */
        digitalWrite(APINS[y], y == x ? HIGH : LOW);
        delay(1);
    }
    drive_all(-1);
    reply("RESULT MAP x=%lu%s", (unsigned long)x, text);
}

/**
 * @brief Read a steady pin at several resolutions, interleaved, and check zero padding above
 * 12 bits; then show that invalid resolution requests keep the previous setting.
 */
static void cmd_res(uint32_t pin, uint32_t n)
{
    static const int BITS[] = {12, 10, 8, 1, 14, 16, 31};
    Stats s[7];
    uint32_t pad_bad = 0;
    uint32_t negative = 0;

    for (uint32_t i = 0; i < n; i++) {
        for (size_t k = 0; k < 7; k++) {
            analogReadResolution(BITS[k]);
            int v = analogRead(pin);
            if (v < 0) {
                negative++;
                continue;
            }
            if (BITS[k] > 12 && (v & ((1 << (BITS[k] - 12)) - 1)) != 0) {
                pad_bad++;
            }
            /* Compare in 12-bit units; keep 31-bit values in range. */
            s[k].add(BITS[k] > 12 ? v >> (BITS[k] - 12) : v);
        }
    }
    Stats inv;
    analogReadResolution(12);
    for (int bad : {0, 32, -1, 100}) {
        analogReadResolution(bad);
        inv.add(analogRead(pin));
    }
    analogReadResolution(10);
    reply("RESULT RES pin=%lu n=%lu neg=%lu pad_bad=%lu m12=%ld m10=%ld m8=%ld m1=%ld m14=%ld m16=%ld m31=%ld "
          "max1=%ld inv_mean_x100=%ld",
          (unsigned long)pin, (unsigned long)n, (unsigned long)negative, (unsigned long)pad_bad, s[0].mean_x100(),
          s[1].mean_x100(), s[2].mean_x100(), s[3].mean_x100(), s[4].mean_x100(), s[5].mean_x100(),
          s[6].mean_x100(), (long)s[3].hi, inv.mean_x100());
}

/**
 * @brief Sample A2/A3 at 12 bits and VBAT/2 through LHAL (exploratory), n samples each.
 */
static void sample(uint32_t n, Stats &a2, Stats &a3, Stats &vbat, uint32_t &fail)
{
    struct bflb_device_s *adc = bflb_device_get_by_name("adc_v2_0");

    analogReadResolution(12);
    for (uint32_t i = 0; i < n; i++) {
        int v2 = analogRead(A2);
        int v3 = analogRead(A3);
        fail += (v2 < 0) + (v3 < 0);
        if (v2 >= 0) {
            a2.add(v2);
        }
        if (v3 >= 0) {
            a3.add(v3);
        }
        if (i % 4 == 0) { /* VBAT/2 raw 16-bit field: supply ripple plus ADC noise. */
            struct bflb_adc_channel_s chan = {ADC_INTERNAL_CHANNEL_VBAT_HALF, ADC_INTERNAL_CHANNEL_AVSS};
            bflb_adc_feature_control(adc, ADC_CMD_VBAT_EN, 1);
            bflb_adc_channel_config_internal(adc, &chan, 1);
            bflb_adc_clear_fifo(adc);
            bflb_adc_start_conversion(adc);
            while (bflb_adc_get_count(adc) == 0) {
            }
            vbat.add((int32_t)(bflb_adc_read_raw(adc) & 0xFFFF));
            bflb_adc_stop_conversion(adc);
            bflb_adc_feature_control(adc, ADC_CMD_VBAT_EN, 0);
        }
    }
    analogReadResolution(10);
}

/**
 * @brief Steady-input noise at 12 bits on A2/A3; supports the 12 effective-bit limit.
 */
static void cmd_noise(uint32_t n)
{
    Stats a2, a3, vbat;
    uint32_t fail = 0;
    sample(n, a2, a3, vbat, fail);
    reply("RESULT NOISE n=%lu fail=%lu a2_mean_x100=%ld a2_sd_x100=%ld a2_min=%ld a2_max=%ld a3_mean_x100=%ld "
          "a3_sd_x100=%ld a3_min=%ld a3_max=%ld vbat_mean_x100=%ld vbat_sd_x100=%ld",
          (unsigned long)n, (unsigned long)fail, a2.mean_x100(), a2.sd_x100(), (long)a2.lo, (long)a2.hi,
          a3.mean_x100(), a3.sd_x100(), (long)a3.lo, (long)a3.hi, vbat.mean_x100(), vbat.sd_x100());
}

/**
 * @brief Scan repeatedly until scan_run clears, as background Wi-Fi activity.
 */
static void scan_task(void *arg)
{
    (void)arg;
    while (scan_run) {
        scan_found = WiFi.scanNetworks();
        scan_count = scan_count + 1;
    }
    scan_done = true;
    vTaskDelete(nullptr);
}

/**
 * @brief Sample with Wi-Fi idle (0), a running soft AP (1) or continuous STA scans (2).
 */
static void cmd_wifi(uint32_t mode, uint32_t n)
{
    Stats a2, a3, vbat;
    uint32_t fail = 0;
    int ap_status = -1;

    scan_count = 0;
    scan_found = -99;
    if (mode == 1) {
        ap_status = WiFi.beginAP("bl-adc-test", "adc-test-only", 6);
        delay(2000);
    } else if (mode == 2) {
        scan_run = true;
        scan_done = false;
        xTaskCreate(scan_task, "scan", 4096 / sizeof(StackType_t), nullptr, 2, nullptr);
        delay(500);
    }
    uint32_t t0 = millis();
    sample(n, a2, a3, vbat, fail);
    uint32_t ms = millis() - t0;
    int status = WiFi.status();
    if (mode == 1) {
        WiFi.end();
    } else if (mode == 2) {
        scan_run = false;
        while (!scan_done) {
            delay(10);
        }
        WiFi.end();
    }
    reply("RESULT WIFI mode=%lu n=%lu ms=%lu fail=%lu ap_status=%d status=%d scans=%lu found=%d a2_mean_x100=%ld "
          "a2_sd_x100=%ld a2_min=%ld a2_max=%ld a3_mean_x100=%ld a3_sd_x100=%ld a3_min=%ld a3_max=%ld "
          "vbat_mean_x100=%ld vbat_sd_x100=%ld vbat_min=%ld vbat_max=%ld",
          (unsigned long)mode, (unsigned long)n, (unsigned long)ms, (unsigned long)fail, ap_status, status,
          (unsigned long)scan_count, scan_found, a2.mean_x100(), a2.sd_x100(), (long)a2.lo, (long)a2.hi,
          a3.mean_x100(), a3.sd_x100(), (long)a3.lo, (long)a3.hi, vbat.mean_x100(), vbat.sd_x100(),
          (long)vbat.lo, (long)vbat.hi);
}

/**
 * @brief Non-ADC and reserved pins return -1 and keep their digital state; analogReference()
 * accepts AR_DEFAULT and ignores other modes.
 */
static void cmd_invalid(void)
{
    static const int PINS[] = {8, 9, 10, 11, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
                               25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 64, 255};
    uint32_t checked = 0;
    uint32_t not_minus1 = 0;
    int first_bad = -1;

    for (int p : PINS) { /* Includes Serial TX/RX 34/35: a rejected pin must stay untouched. */
        int v = analogRead((pin_size_t)p);
        checked++;
        if (v != -1) {
            not_minus1++;
            first_bad = first_bad < 0 ? p : first_bad;
        }
    }
    pinMode(9, OUTPUT);
    digitalWrite(9, HIGH);
    int r9 = analogRead(9);
    int level9 = digitalRead(9);
    digitalWrite(9, LOW);
    int level9_low = digitalRead(9);
    pinMode(9, INPUT);

    Stats def, other;
    analogReadResolution(12);
    analogReference(AR_DEFAULT);
    for (int i = 0; i < 200; i++) {
        def.add(analogRead(A2));
    }
    analogReference(1);
    for (int i = 0; i < 200; i++) {
        other.add(analogRead(A2));
    }
    analogReference(AR_DEFAULT);
    analogReadResolution(10);
    reply("RESULT INVALID checked=%lu not_minus1=%lu first_bad=%d r9=%d level9_high=%d level9_low=%d "
          "ref_default_mean_x100=%ld ref_other_mean_x100=%ld",
          (unsigned long)checked, (unsigned long)not_minus1, first_bad, r9, level9, level9_low, def.mean_x100(),
          other.mean_x100());
}

/**
 * @brief After analogRead(), pinMode() returns the pin to digital use, and analogRead() works again.
 */
static void cmd_restore(void)
{
    analogRead(A0);
    pinMode(A0, OUTPUT);
    digitalWrite(A0, HIGH);
    int high = digitalRead(A0);
    digitalWrite(A0, LOW);
    int low = digitalRead(A0);
    pinMode(A0, INPUT_PULLUP);
    delay(1);
    int pullup = digitalRead(A0);
    int again = analogRead(A0);
    pinMode(A0, INPUT);
    reply("RESULT RESTORE high=%d low=%d pullup=%d again=%d", high, low, pullup, again);
}

/**
 * @brief Exploratory: drive each A pin as an output and convert its channel through LHAL, so the
 * pin stays at the rail during conversion. Reports calibrated 16-bit values.
 */
static void cmd_rails(void)
{
    struct bflb_device_s *adc = bflb_device_get_by_name("adc_v2_0");
    struct bflb_device_s *gpio = bflb_device_get_by_name("gpio");
    char text[400];
    size_t len = 0;

    analogRead(A0); /* Ensure the Core initialized and calibrated the ADC. */
    for (size_t i = 0; i < NA; i++) {
        int32_t v[2];
        for (int level = 1; level >= 0; level--) {
            bflb_gpio_init(gpio, APINS[i], GPIO_OUTPUT | GPIO_INPUT | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_1);
            if (level) {
                bflb_gpio_set(gpio, APINS[i]);
            } else {
                bflb_gpio_reset(gpio, APINS[i]);
            }
            delay(1);
            struct bflb_adc_channel_s chan = {ACHANNELS[i], ADC_EXTERNAL_CHANNEL_NULL};
            bflb_adc_channel_config_external(adc, &chan, 1);
            bflb_adc_clear_fifo(adc);
            bflb_adc_start_conversion(adc);
            while (bflb_adc_get_count(adc) == 0) {
            }
            uint32_t raw = bflb_adc_read_raw(adc);
            bflb_adc_stop_conversion(adc);
            struct bflb_adc_result_s r;
            bflb_adc_parse_result(adc, &raw, &r, 1);
            v[level] = r.value;
        }
        pinMode(APINS[i], INPUT);
        len += snprintf(text + len, sizeof(text) - len, " hi%u=%ld lo%u=%ld", (unsigned)i, (long)v[1], (unsigned)i,
                        (long)v[0]);
    }
    reply("RESULT RAILS%s", text);
}

/**
 * @brief Split a command line into words and run it.
 */
static void dispatch(char *line)
{
    char *argv[3] = {};
    size_t argc = 0;
    for (char *tok = strtok(line, " "); tok != nullptr && argc < 3; tok = strtok(nullptr, " ")) {
        argv[argc++] = tok;
    }
    if (argc == 0) {
        return;
    }
    uint32_t a1 = argc > 1 ? strtoul(argv[1], nullptr, 0) : 0;
    uint32_t a2 = argc > 2 ? strtoul(argv[2], nullptr, 0) : 0;
    if (strcmp(argv[0], "INFO") == 0) {
        cmd_info();
    } else if (strcmp(argv[0], "MAP") == 0) {
        cmd_map(a1);
    } else if (strcmp(argv[0], "RES") == 0) {
        cmd_res(a1, a2);
    } else if (strcmp(argv[0], "NOISE") == 0) {
        cmd_noise(a1);
    } else if (strcmp(argv[0], "WIFI") == 0) {
        cmd_wifi(a1, a2);
    } else if (strcmp(argv[0], "INVALID") == 0) {
        cmd_invalid();
    } else if (strcmp(argv[0], "RESTORE") == 0) {
        cmd_restore();
    } else if (strcmp(argv[0], "RAILS") == 0) {
        cmd_rails();
    } else {
        reply("RESULT ERROR unknown=%s", argv[0]);
    }
}

void setup()
{
    /* The first analogRead() initializes and calibrates the ADC; read at the default resolution. */
    uint32_t t0 = micros();
    boot_value = analogRead(A2);
    boot_us = micros() - t0;
    Serial.begin(115200);
    reply("READY AdcAcceptance");
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
