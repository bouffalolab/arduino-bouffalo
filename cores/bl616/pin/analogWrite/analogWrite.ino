#include <stdio.h>

void setup() {
    // put your setup code here, to run once:
    pinMode(2, OUTPUT);
    pinMode(3, OUTPUT);
}

uint32_t i = 0;
void loop() {
    // put your main code here, to run repeatedly:
    int val2 = 4095;
    int val3 = 2048;
    analogWrite(2, val2);
    analogWrite(3, val3);
    printf("hellow world %d, pin2: %d, pin3: %d\r\n", i, val2, val3);
    i++;
    delay(1000);
}
