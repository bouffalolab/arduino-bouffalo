/*
 * HardwareSerial hardware acceptance probe for Ai-M64P-32S-Kit.
 *
 * Serial (UART0, onboard CH340) carries line commands from tests/hardware/uart_acceptance.py at
 * 115200 8N1; every command answers with one "RESULT ..." line that the host script judges.
 * Serial1 (UART1) needs a jumper from GPIO14 (TX) to GPIO15 (RX).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "bflb_clock.h"

#if !defined(ARDUINO_AI_M64P_32S_KIT)
#error "UartAcceptance needs Ai-M64P-32S-Kit: Serial on the CH340 and a GPIO14-GPIO15 jumper."
#endif

static const unsigned long CONTROL_BAUD = 115200;
static const pin_size_t LOOP_TX = PIN_SERIAL1_TX;
static const pin_size_t LOOP_RX = PIN_SERIAL1_RX;
static const int MAX_EDGES = 64;
static const size_t MAX_ARGS = 8;

/* xorshift32 stream; the host script implements the same generator. */
struct Prng {
    uint32_t s;
    explicit Prng(uint32_t seed) : s(seed != 0 ? seed : 1) {}
    uint8_t next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return (uint8_t)s;
    }
};

static char line_buf[128];
static size_t line_len;
static uint32_t cpu_hz;
static uint32_t edge_t[MAX_EDGES];
static uint8_t rx_copy[4096];

/**
 * @brief Read the low word of the machine cycle counter.
 */
static inline uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile("csrr %0, mcycle" : "=r"(c));
    return c;
}

/**
 * @brief Return the value mask for the data-bit field of an Arduino SERIAL_* configuration.
 */
static uint8_t data_mask(uint16_t config)
{
    switch (config & SERIAL_DATA_MASK) {
        case SERIAL_DATA_5: return 0x1f;
        case SERIAL_DATA_6: return 0x3f;
        case SERIAL_DATA_7: return 0x7f;
        default: return 0xff;
    }
}

/**
 * @brief Print one formatted line on the control port.
 */
static void reply(const char *fmt, ...)
{
    char out[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, sizeof(out), fmt, ap);
    va_end(ap);
    Serial.println(out);
}

/**
 * @brief Discard bytes already received on Serial1.
 */
static void drain_loop_rx(void)
{
    while (Serial1.available() > 0) {
        Serial1.read();
    }
}

/**
 * @brief Measure the mcycle rate against micros().
 */
static uint32_t measure_cpu_hz(void)
{
    uint32_t c0 = cycles();
    uint32_t u0 = micros();
    delay(200);
    uint32_t c1 = cycles();
    uint32_t u1 = micros();
    return (uint32_t)((uint64_t)(c1 - c0) * 1000000ull / (u1 - u0));
}

/**
 * @brief Report clocks used by the host to derive expected bit periods.
 */
static void cmd_info(void)
{
    reply("RESULT INFO uart0_clk=%lu uart1_clk=%lu cpu_hz=%lu f_cpu=%lu",
          (unsigned long)bflb_clk_get_peripheral_clock(BFLB_DEVICE_TYPE_UART, 0),
          (unsigned long)bflb_clk_get_peripheral_clock(BFLB_DEVICE_TYPE_UART, 1),
          (unsigned long)cpu_hz, (unsigned long)F_CPU);
}

/**
 * @brief Prove the GPIO14-GPIO15 jumper: a HIGH read against the RX pull-down needs a driver.
 */
static void cmd_jumper(void)
{
    Serial1.end();
    pinMode(LOOP_TX, INPUT);
    pinMode(LOOP_RX, INPUT_PULLDOWN);
    delay(1);
    int idle = digitalRead(LOOP_RX);
    pinMode(LOOP_TX, OUTPUT);
    int bad = 0;
    char seen[9] = {};
    for (int i = 0; i < 8; i++) {
        PinStatus level = (i & 1) ? HIGH : LOW;
        digitalWrite(LOOP_TX, level);
        delayMicroseconds(50);
        int got = digitalRead(LOOP_RX);
        seen[i] = got ? '1' : '0';
        bad += got != level;
    }
    pinMode(LOOP_TX, INPUT);
    reply("RESULT JUMPER idle=%d seen=%s bad=%d", idle, seen, bad);
}

/**
 * @brief Send n masked pseudo-random bytes through the Serial1 loopback and compare them.
 */
static void cmd_loop(unsigned long baud, uint16_t config, size_t n)
{
    Serial1.begin(baud, config);
    if (!Serial1) {
        reply("RESULT LOOP baud=%lu cfg=0x%x begin=0", baud, config);
        return;
    }
    delay(2);
    drain_loop_rx();
    uint8_t mask = data_mask(config);
    Prng tx(0x9e3779b9u ^ baud ^ config);
    Prng rx(0x9e3779b9u ^ baud ^ config);
    size_t sent = 0;
    size_t got = 0;
    size_t mismatch = 0;
    long first_bad = -1;
    uint32_t idle_limit = 50000 + (uint32_t)(20ull * 13 * 1000000ull / baud); /* 50 ms + 20 frames */
    uint32_t t0 = micros();
    uint32_t last = t0;
    while (got < n && micros() - last < idle_limit) {
        /* Keep at most 128 bytes in flight so a 256-byte RX ring cannot fill between reads. */
        while (sent < n && sent - got < 128 && Serial1.availableForWrite() > 0) {
            Serial1.write((uint8_t)(tx.next() & mask));
            sent++;
        }
        while (Serial1.available() > 0) {
            int c = Serial1.read();
            uint8_t want = rx.next() & mask;
            if (c != want) {
                if (first_bad < 0) {
                    first_bad = (long)got;
                }
                mismatch++;
            }
            got++;
            last = micros();
        }
    }
    uint32_t us = micros() - t0;
    reply("RESULT LOOP baud=%lu cfg=0x%x begin=1 n=%u sent=%u got=%u mismatch=%u first_bad=%ld "
          "overflow=%lu us=%lu",
          baud, config, (unsigned)n, (unsigned)sent, (unsigned)got, (unsigned)mismatch, first_bad,
          (unsigned long)Serial1.rxOverflowCount(), (unsigned long)us);
    Serial1.end();
}

/**
 * @brief Capture Serial1 TX edges on GPIO15 as a GPIO input and report cycle timestamps.
 *
 * The first byte is a lead-in whose start edge may precede sampling; the host decodes the rest.
 */
static void cmd_frame(unsigned long baud, uint16_t config, const uint8_t *bytes, size_t count)
{
    Serial1.begin(baud, config);
    if (!Serial1) {
        reply("RESULT FRAME baud=%lu cfg=0x%x begin=0", baud, config);
        return;
    }
    pinMode(LOOP_RX, INPUT); /* GLB input does not follow a pad muxed to UART RX. */
    delay(5);
    uint8_t mask = data_mask(config);
    uint32_t budget = (uint32_t)((uint64_t)cpu_hz * (count + 1) * 14 / baud);
    int n = 0;
    taskENTER_CRITICAL();
    int prev = digitalRead(LOOP_RX);
    int init = prev;
    uint32_t t0 = cycles();
    Serial1.write((uint8_t)(0x55 & mask));
    for (size_t i = 0; i < count; i++) {
        Serial1.write((uint8_t)(bytes[i] & mask));
    }
    while ((uint32_t)(cycles() - t0) < budget && n < MAX_EDGES) {
        int level = digitalRead(LOOP_RX);
        if (level != prev) {
            edge_t[n++] = cycles() - t0;
            prev = level;
        }
    }
    taskEXIT_CRITICAL();
    Serial1.end();
    Serial.print("RESULT FRAME baud=");
    Serial.print(baud);
    Serial.print(" cfg=0x");
    Serial.print(config, HEX);
    Serial.print(" begin=1 init=");
    Serial.print(init);
    Serial.print(" edges=");
    Serial.print(n);
    Serial.print(" t=");
    for (int i = 0; i < n; i++) {
        if (i > 0) {
            Serial.print(',');
        }
        Serial.print(edge_t[i]);
    }
    Serial.println();
}

/**
 * @brief Check that flush() returns only after the queued bytes have left the transmitter.
 */
static void cmd_flush(unsigned long baud)
{
    Serial1.begin(baud, SERIAL_8N1);
    if (!Serial1) {
        reply("RESULT FLUSH baud=%lu begin=0", baud);
        return;
    }
    delay(2);
    drain_loop_rx();
    int fifo = Serial1.availableForWrite();
    uint8_t buf[32];
    size_t len = (size_t)fifo < sizeof(buf) ? (size_t)fifo : sizeof(buf);
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(0xa0 + i);
    }
    uint32_t t0 = micros();
    size_t w = Serial1.write(buf, len);
    uint32_t t1 = micros();
    Serial1.flush();
    uint32_t t2 = micros();
    int afw = Serial1.availableForWrite();
    delay(2 + 100 * 1000 / baud);
    int rx = Serial1.available();
    reply("RESULT FLUSH baud=%lu begin=1 fifo=%d len=%u w=%u write_us=%lu flush_us=%lu afw=%d rx=%d",
          baud, fifo, (unsigned)len, (unsigned)w, (unsigned long)(t1 - t0),
          (unsigned long)(t2 - t0), afw, rx);
    Serial1.end();
}

/**
 * @brief Fill a 64-byte RX ring without reading and check overflow accounting and retention.
 */
static void cmd_ovf(void)
{
    Serial1.end();
    bool resized = Serial1.setRxBufferSize(64);
    Serial1.begin(115200, SERIAL_8N1);
    bool active_resize = Serial1.setRxBufferSize(128); /* Must be refused while active. */
    delay(2);
    drain_loop_rx();
    for (int i = 0; i < 200; i++) {
        Serial1.write((uint8_t)i);
    }
    Serial1.flush();
    delay(5);
    int avail = Serial1.available();
    unsigned long ovf = Serial1.rxOverflowCount();
    int first_bad = -1;
    for (int i = 0; i < avail; i++) {
        if (Serial1.read() != i && first_bad < 0) {
            first_bad = i;
        }
    }
    Serial1.end();
    bool restored = Serial1.setRxBufferSize(256);
    reply("RESULT OVF resized=%d active_resize=%d avail=%d overflow=%lu first_bad=%d restored=%d",
          resized, active_resize, avail, ovf, first_bad, restored);
}

/**
 * @brief Check peek/read/available semantics on a short loopback message.
 */
static void cmd_api(void)
{
    Serial1.begin(115200, SERIAL_8N1);
    delay(2);
    drain_loop_rx();
    size_t zero = Serial1.write((const uint8_t *)"", 0);
    Serial1.write((const uint8_t *)"ABC", 3);
    Serial1.flush();
    delay(2);
    int a0 = Serial1.available();
    int p0 = Serial1.peek();
    int p1 = Serial1.peek();
    int a1 = Serial1.available();
    int r0 = Serial1.read();
    int r1 = Serial1.read();
    int r2 = Serial1.read();
    int r3 = Serial1.read();
    int p2 = Serial1.peek();
    bool open = (bool)Serial1;
    Serial1.end();
    reply("RESULT API zero=%u a0=%d p0=%d p1=%d a1=%d r=%d,%d,%d,%d p2=%d open=%d", (unsigned)zero,
          a0, p0, p1, a1, r0, r1, r2, r3, p2, open);
}

/**
 * @brief Check that end() frees the pins for GPIO use and begin() restores the loopback.
 */
static void cmd_end(void)
{
    Serial1.begin(115200, SERIAL_8N1);
    delay(2);
    drain_loop_rx();
    Serial1.write((const uint8_t *)"pre", 3);
    Serial1.flush();
    delay(2);
    int pre = Serial1.available();
    Serial1.end();
    bool closed = !Serial1;
    size_t w_closed = Serial1.write('x');
    int afw_closed = Serial1.availableForWrite();
    int avail_closed = Serial1.available();
    pinMode(LOOP_TX, OUTPUT);
    pinMode(LOOP_RX, INPUT_PULLDOWN);
    int bad = 0;
    for (int i = 0; i < 6; i++) {
        PinStatus level = (i & 1) ? HIGH : LOW;
        digitalWrite(LOOP_TX, level);
        delayMicroseconds(50);
        bad += digitalRead(LOOP_RX) != level;
    }
    Serial1.begin(115200, SERIAL_8N1);
    delay(2);
    drain_loop_rx();
    Serial1.write((const uint8_t *)"post", 4);
    Serial1.flush();
    delay(2);
    int post = Serial1.available();
    Serial1.end();
    reply("RESULT END pre=%d closed=%d w_closed=%u afw_closed=%d avail_closed=%d gpio_bad=%d post=%d",
          pre, closed, (unsigned)w_closed, afw_closed, avail_closed, bad, post);
}

/**
 * @brief Report whether begin() accepts a configuration.
 */
static void cmd_begin(unsigned long baud, uint16_t config)
{
    Serial1.begin(baud, config);
    bool ok = (bool)Serial1;
    size_t w = Serial1.write('U');
    Serial1.end();
    reply("RESULT BEGIN baud=%lu cfg=0x%x ok=%d w=%u", baud, config, ok, (unsigned)w);
}

/**
 * @brief Read one byte from Serial with a deadline; return -1 on timeout.
 */
static int read_byte(uint32_t timeout_ms)
{
    uint32_t t0 = millis();
    while (millis() - t0 < timeout_ms) {
        if (Serial.available() > 0) {
            return Serial.read();
        }
    }
    return -1;
}

/**
 * @brief Exchange n bytes with the CH340 after switching UART0 to the requested format.
 *
 * The host switches its port after "SWITCH", sends a sync byte, reads the DUT pattern, then sends
 * its own pattern. The DUT returns to 115200 8N1 before reporting.
 */
static void cmd_u0(unsigned long baud, uint16_t config, size_t n)
{
    uint8_t mask = data_mask(config);
    reply("SWITCH baud=%lu cfg=0x%x n=%u", baud, config, (unsigned)n);
    Serial.flush();
    Serial.begin(baud, config);
    bool ok = (bool)Serial;
    int sync = -2;
    size_t got = 0;
    size_t mismatch = 0;
    unsigned long ovf = 0;
    if (ok) {
        sync = read_byte(3000);
        if (sync == (0x0a & mask)) {
            Prng tx(0x51ed2705u ^ baud ^ config);
            for (size_t i = 0; i < n; i++) {
                Serial.write((uint8_t)(tx.next() & mask));
            }
            Serial.flush();
            Prng rx(0x2545f491u ^ baud ^ config);
            uint32_t last = millis();
            while (got < n && millis() - last < 3000) {
                while (Serial.available() > 0) {
                    rx_copy[got % sizeof(rx_copy)] = (uint8_t)Serial.read();
                    mismatch += rx_copy[got % sizeof(rx_copy)] != (rx.next() & mask);
                    got++;
                    last = millis();
                }
            }
            ovf = Serial.rxOverflowCount();
        }
        delay(100);
    }
    Serial.begin(CONTROL_BAUD, SERIAL_8N1);
    delay(200);
    reply("RESULT U0 baud=%lu cfg=0x%x begin=%d sync=%d n=%u got=%u mismatch=%u overflow=%lu", baud,
          config, ok, sync, (unsigned)n, (unsigned)got, (unsigned)mismatch, ovf);
}

/**
 * @brief Split the command line and run the matching check.
 */
static void dispatch(char *line)
{
    char *argv[MAX_ARGS];
    size_t argc = 0;
    for (char *tok = strtok(line, " "); tok != nullptr && argc < MAX_ARGS; tok = strtok(nullptr, " ")) {
        argv[argc++] = tok;
    }
    if (argc == 0) {
        return;
    }
    unsigned long a[MAX_ARGS] = {};
    for (size_t i = 1; i < argc; i++) {
        a[i] = strtoul(argv[i], nullptr, 0);
    }
    if (!strcmp(argv[0], "INFO")) {
        cmd_info();
    } else if (!strcmp(argv[0], "JUMPER")) {
        cmd_jumper();
    } else if (!strcmp(argv[0], "LOOP") && argc == 4) {
        cmd_loop(a[1], (uint16_t)a[2], a[3]);
    } else if (!strcmp(argv[0], "FRAME") && argc >= 4) {
        uint8_t bytes[MAX_ARGS];
        for (size_t i = 3; i < argc; i++) {
            bytes[i - 3] = (uint8_t)a[i];
        }
        cmd_frame(a[1], (uint16_t)a[2], bytes, argc - 3);
    } else if (!strcmp(argv[0], "FLUSH") && argc == 2) {
        cmd_flush(a[1]);
    } else if (!strcmp(argv[0], "OVF")) {
        cmd_ovf();
    } else if (!strcmp(argv[0], "API")) {
        cmd_api();
    } else if (!strcmp(argv[0], "END")) {
        cmd_end();
    } else if (!strcmp(argv[0], "BEGIN") && argc == 3) {
        cmd_begin(a[1], (uint16_t)a[2]);
    } else if (!strcmp(argv[0], "U0") && argc == 4 && a[3] <= sizeof(rx_copy)) {
        cmd_u0(a[1], (uint16_t)a[2], a[3]);
    } else {
        reply("RESULT ERROR unknown=%s", argv[0]);
    }
}

/**
 * @brief Start the control port and announce readiness.
 */
void setup()
{
    Serial.begin(CONTROL_BAUD);
    cpu_hz = measure_cpu_hz();
    reply("READY UartAcceptance cpu_hz=%lu", (unsigned long)cpu_hz);
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
