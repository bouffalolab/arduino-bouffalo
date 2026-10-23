// random、randomSeed 检查：同一种子得到同一序列，random(n) 落在 [0, n)，random(a, b) 落在 [a, b)，
// 边界情况同官方 Core（上限 0 返回 0，下限不小于上限返回下限）。每秒输出一行 "WMATH <失败项数> <样本>"，0 为通过
static int failures;

static void expect(bool ok)
{
    failures += ok ? 0 : 1;
}

static void check()
{
    long first[8];

    failures = 0;
    randomSeed(42);
    for (int i = 0; i < 8; i++) {
        first[i] = random(1000);
    }
    randomSeed(42);
    for (int i = 0; i < 8; i++) {
        expect(random(1000) == first[i]);
    }
    bool varied = false;
    for (int i = 1; i < 8; i++) {
        varied = varied || first[i] != first[0];
    }
    expect(varied);
    for (int i = 0; i < 1000; i++) {
        long a = random(10);
        long b = random(-5, 5);
        expect(a >= 0 && a < 10);
        expect(b >= -5 && b < 5);
    }
    expect(random(0) == 0);
    expect(random(7, 7) == 7);
    expect(random(9, 3) == 9);
}

void setup()
{
    Serial.begin(115200);
}

void loop()
{
    check();
    Serial.print("WMATH ");
    Serial.print(failures);
    Serial.print(' ');
    Serial.println(random(1000));
    delay(1000);
}
