// Echo Serial1 hardware-UART input and print uptime each second to check millis() and TX.
static unsigned long last;

/**
 * @brief Initialize the resources used by this example or validation sketch.
 */
void setup()
{
    Serial1.begin(115200);
    Serial1.println("Serial1Echo ready");
}

/**
 * @brief Service the example or validation scenario on each Arduino loop iteration.
 */
void loop()
{
    while (Serial1.available() > 0) {
        Serial1.write(Serial1.read());
    }
    if (millis() - last >= 1000) {
        last = millis();
        Serial1.print("uptime ms: ");
        Serial1.println(last);
    }
}
