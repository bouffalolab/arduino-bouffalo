// Check repeatable randomSeed sequences and random(n) in [0,n), random(a,b) in [a,b).
// Match official boundary behavior: zero upper bound returns zero; an empty interval returns its lower bound. Print "WMATH <failures> <sample>" each second; zero means pass.
static int failures;

/**
 * @brief Count and report a failed sketch expectation.
 */
static void expect(bool ok)
{
    failures += ok ? 0 : 1;
}

/**
 * @brief Validate the selected artifact or fixture and raise an error on a failed invariant.
 */
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

/**
 * @brief Initialize the resources used by this example or validation sketch.
 */
void setup()
{
    Serial.begin(115200);
}

/**
 * @brief Service the example or validation scenario on each Arduino loop iteration.
 */
void loop()
{
    check();
    Serial.print("WMATH ");
    Serial.print(failures);
    Serial.print(' ');
    Serial.println(random(1000));
    delay(1000);
}
