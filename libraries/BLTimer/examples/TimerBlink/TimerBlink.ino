/*
 * Blink LED_BUILTIN from a 500 ms periodic BLTimer and report a 3 s one-shot from loop().
 * Callbacks run in interrupt context: keep them short and hand work to loop() through a flag.
 */
#include <BLTimer.h>

BLTimer blinkTimer;
BLTimer onceTimer;
volatile bool onceFired = false;

void toggleLed()
{
    static bool on = false;
    on = !on;
    digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
}

void markOnce()
{
    onceFired = true;
}

void setup()
{
    Serial.begin(115200);
    pinMode(LED_BUILTIN, OUTPUT);
    if (!blinkTimer.begin(toggleLed, 500000)) {
        Serial.println("No free timer for blinking");
    }
    if (!onceTimer.beginOnce(markOnce, 3000000)) {
        Serial.println("No free timer for the one-shot");
    }
}

void loop()
{
    if (onceFired) {
        onceFired = false;
        Serial.print("One-shot fired on TIMER");
        Serial.println(onceTimer.timer());
        onceTimer.end();
    }
}
