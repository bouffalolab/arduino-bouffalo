#include <stdio.h>

void setup() {
    // put your setup code here, to run once:
    pinMode(34, INPUT);
}

uint32_t i = 0;
void loop() {
    // put your main code here, to run repeatedly:
    int val = digitalRead(34);
    printf("hellow world %d, val = %d\r\n", i, val);
    i++;
    delay(1000);
}
