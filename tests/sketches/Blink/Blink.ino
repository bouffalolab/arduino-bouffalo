/**
 * @brief The onboard LED is active-low (see variant), reversing the usual Blink on/off levels.
 */
void setup()
{
    pinMode(LED_BUILTIN, OUTPUT);
}

/**
 * @brief Service the example or validation scenario on each Arduino loop iteration.
 */
void loop()
{
    digitalWrite(LED_BUILTIN, LOW);
    delay(500);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(500);
}
