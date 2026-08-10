#include "Arduino.h"

void setup()
{
    pinMode(LED_BUILTIN, OUTPUT);
    Serial.begin(2000000);
    Serial.println("BL616CL Arduino Blink");
}

void loop()
{
    digitalWrite(LED_BUILTIN, HIGH);
    delay(500);
    digitalWrite(LED_BUILTIN, LOW);
    delay(500);
}
