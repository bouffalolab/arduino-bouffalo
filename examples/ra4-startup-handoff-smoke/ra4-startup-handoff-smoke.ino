// Isolate application startup and CDC/UART delivery from radio initialization.
char request[48];
unsigned used;
unsigned long next_ready;
void setup() { Serial.begin(115200); }
void loop() {
  if ((long)(millis() - next_ready) >= 0) {
    Serial.println("RA4_STARTUP:READY");
    next_ready = millis() + 100;
  }
  while (Serial.available()) {
    char value = Serial.read();
    if (value == '\n') {
      request[used] = 0;
      Serial.print("RA4_STARTUP:ECHO="); Serial.println(request);
      used = 0;
    } else if (used + 1 < sizeof(request)) request[used++] = value;
  }
}
