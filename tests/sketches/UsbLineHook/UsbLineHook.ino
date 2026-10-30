// Check Serial.setTouchReset(), Serial.onLineChange() and resetToBootloader().
// USB is repeatedly opened/closed during the check; use Serial1 at 115200 for commands and results.
// Read line state, delay(20), then record one callback. loop prints "CB <sequence> <baud> <DTR> <RTS> <measured-delay-ms>".
// Serial1 commands '0'/'1' disable/enable touch and reply "TOUCH 0/1"; 'D' replies "BOOT" and enters ROM.
// '?' replies "STAT <callback-count> <touch>". Touch is disabled at startup.
struct Entry {
    uint32_t seq;
    uint32_t baud;
    bool dtr;
    bool rts;
    uint32_t ms;
};

static Entry ring[64];
static volatile uint32_t head; // Written by the callback.
static uint32_t tail;          // Written by loop.
static volatile uint32_t dropped;
static bool touch;

/**
 * @brief Print the observed USB host line settings from the test callback.
 */
static void onLine()
{
    Entry e;
    e.seq = head + dropped + 1;
    e.baud = Serial.baud();
    e.dtr = Serial.dtr();
    e.rts = Serial.rts();
    unsigned long start = millis();
    delay(20);
    e.ms = millis() - start;
    if (head - tail >= sizeof(ring) / sizeof(ring[0])) {
        dropped = dropped + 1;
        return;
    }
    ring[head % (sizeof(ring) / sizeof(ring[0]))] = e;
    head = head + 1;
}

/**
 * @brief Toggle touch-reset handling and report the selected test policy.
 */
static void setTouch(bool enable)
{
    touch = enable;
    Serial.setTouchReset(enable);
}

/**
 * @brief Initialize the resources used by this example or validation sketch.
 */
void setup()
{
    Serial1.begin(115200);
    setTouch(false);
    bool hooked = Serial.onLineChange(onLine);
    Serial1.print("UsbLineHook ready hook=");
    Serial1.println(hooked ? 1 : 0);
}

/**
 * @brief Service the example or validation scenario on each Arduino loop iteration.
 */
void loop()
{
    while (tail != head) {
        Entry e = ring[tail % (sizeof(ring) / sizeof(ring[0]))];
        tail++;
        Serial1.print("CB ");
        Serial1.print(e.seq);
        Serial1.print(' ');
        Serial1.print(e.baud);
        Serial1.print(' ');
        Serial1.print(e.dtr ? 1 : 0);
        Serial1.print(' ');
        Serial1.print(e.rts ? 1 : 0);
        Serial1.print(' ');
        Serial1.println(e.ms);
    }
    if (Serial1.available() > 0) {
        switch (Serial1.read()) {
            case '0':
                setTouch(false);
                Serial1.println("TOUCH 0");
                break;
            case '1':
                setTouch(true);
                Serial1.println("TOUCH 1");
                break;
            case 'D':
                Serial1.println("BOOT");
                Serial1.flush();
                resetToBootloader();
            case '?':
                Serial1.print("STAT ");
                Serial1.print(head + dropped);
                Serial1.print(' ');
                Serial1.println(touch ? 1 : 0);
                break;
            default:
                break;
        }
    }
}
