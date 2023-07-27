#include <stdio.h>

void setup() {
    // put your setup code here, to run once:
    pinMode(23, OUTPUT);
}

uint32_t i = 0;
void loop() {
    // put your main code here, to run repeatedly:
    printf("hellow world %d\r\n", i);
    i++;
    if (i & 1) {
      digitalWrite(23, HIGH);
    } else {
      digitalWrite(23, LOW);
    }
    delay(1000);
}
