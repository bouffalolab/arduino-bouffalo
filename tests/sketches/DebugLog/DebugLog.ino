// Every 500 ms print "LOG <n>" with printf and "USB <n>" with USB Serial.
// With Debug log enabled, LOG appears only on uart2 (J6 IO41); otherwise it is discarded. USB carries only USB lines in both modes.
#include <stdio.h>

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
    static unsigned long n;
    printf("LOG %lu\r\n", n);
    Serial.print("USB ");
    Serial.println(n);
    n++;
    delay(500);
}
