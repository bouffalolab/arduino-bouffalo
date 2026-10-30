// Echo serial input and print uptime each second to check millis() and TX.
// For native USB Serial, wait until the host opens the port before printing, as official USB-board examples do.
static unsigned long last;

/**
 * @brief Initialize the resources used by this example or validation sketch.
 */
void setup()
{
    Serial.begin(115200);
    while (!Serial) {
    }
    Serial.println("SerialEcho ready");
}

/**
 * @brief Service the example or validation scenario on each Arduino loop iteration.
 */
void loop()
{
    while (Serial.available() > 0) {
        Serial.write(Serial.read());
    }
    if (millis() - last >= 1000) {
        last = millis();
        Serial.print("uptime ms: ");
        Serial.println(last);
    }
}
