
void setup() {
    // put your setup code here, to run once:
    pinMode(33, INPUT_PULLUP);
}

uint32_t i = 0;
void loop() {
    // put your main code here, to run repeatedly:
    int val = digitalRead(33);
    printf("hellow world %d, val = %d\r\n", i, val);
    i++;
    delay(1000);
}
