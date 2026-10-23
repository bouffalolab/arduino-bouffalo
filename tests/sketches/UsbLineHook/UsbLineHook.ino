// USB 串口线路接口检查：Serial.setTouchReset()、Serial.onLineChange()、resetToBootloader()。
// 检查过程中 USB 串口会被反复打开关闭，所以结果与命令都走 Serial1（115200）。
// 回调先读线路状态再 delay(20)，每次记录一条；loop 逐条输出 "CB <序号> <波特率> <DTR> <RTS> <delay 实测 ms>"。
// Serial1 命令：'0'/'1' 关闭/打开 touch（回 "TOUCH 0/1"），'D' 回 "BOOT" 后进下载模式，
// '?' 输出 "STAT <回调次数> <touch>"。启动时关闭 touch。
struct Entry {
    uint32_t seq;
    uint32_t baud;
    bool dtr;
    bool rts;
    uint32_t ms;
};

static Entry ring[64];
static volatile uint32_t head; // 回调写
static uint32_t tail;          // loop 写
static volatile uint32_t dropped;
static bool touch;

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

static void setTouch(bool enable)
{
    touch = enable;
    Serial.setTouchReset(enable);
}

void setup()
{
    Serial1.begin(115200);
    setTouch(false);
    bool hooked = Serial.onLineChange(onLine);
    Serial1.print("UsbLineHook ready hook=");
    Serial1.println(hooked ? 1 : 0);
}

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
