#include "Arduino.h"

struct ConstructorProbe
{
    ConstructorProbe() : value(0x616C) {}
    int value;
};

ConstructorProbe constructor_probe;

void setup()
{
    Serial.begin(2000000);
    Serial.println("BL616CL Arduino Serial");
    Serial.print("constructor=0x");
    Serial.println(constructor_probe.value, HEX);
}

void loop()
{
    static unsigned long sequence;
    Serial.print("millis=");
    Serial.print(millis());
    Serial.print(" sequence=");
    Serial.println(sequence++);
    delay(1000);
}
