#include <WiFiS3.h>
#include <string.h>

// A second board observes one explicitly selected test SSID. Never print SSIDs,
// BSSIDs, credentials, or other networks. START takes an ASCII SSID encoded hex.
namespace {
char command[72] = {}, target[33] = {};
size_t used = 0;
unsigned sequence = 0;
unsigned long next_scan = 0, next_ready = 0;
int digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}
bool select_target() {
  if (strncmp(command, "START ", 6) != 0) return false;
  const char *hex = command + 6;
  const size_t length = strlen(hex);
  if (!length || length > 64 || (length & 1U)) return false;
  char decoded[33] = {};
  for (size_t i = 0; i < length / 2; ++i) {
    const int hi = digit(hex[2*i]), lo = digit(hex[2*i+1]);
    if (hi < 0 || lo < 0) return false;
    const int value = hi * 16 + lo;
    if (value < 32 || value > 126) return false;
    decoded[i] = static_cast<char>(value);
  }
  memcpy(target, decoded, sizeof(target));
  sequence = 0;
  next_scan = millis();
  return true;
}
}
void setup() { Serial.begin(115200); }
void loop() {
  while (Serial.available()) {
    const int value = Serial.read();
    if (value == '\r') continue;
    if (value == '\n') {
      command[used] = 0;
      Serial.println(select_target() ? "AP_OBSERVER:TARGET_READY" : "AP_OBSERVER:COMMAND_REJECTED");
      used = 0;
    } else if (used + 1 < sizeof(command)) {
      command[used++] = static_cast<char>(value);
    } else {
      used = 0;
      target[0] = 0;
      Serial.println("AP_OBSERVER:COMMAND_REJECTED");
    }
  }
  if (!target[0]) {
    if (static_cast<long>(millis() - next_ready) >= 0) {
      Serial.println("AP_OBSERVER:READY");
      next_ready = millis() + 1000;
    }
  } else if (static_cast<long>(millis() - next_scan) >= 0) {
    const int count = WiFi.scanNetworks();
    int matches = 0;
    long strongest = -200;
    for (int i = 0; i < count; ++i) {
      if (WiFi.SSID(i) == String(target)) {
        ++matches;
        const long rssi = WiFi.RSSI(i);
        if (rssi > strongest) strongest = rssi;
      }
    }
    Serial.print("AP_OBSERVER:SCAN="); Serial.print(++sequence);
    Serial.print(",COUNT="); Serial.print(count);
    Serial.print(",MATCHES="); Serial.print(matches);
    if (matches) { Serial.print(",RSSI="); Serial.print(strongest); }
    Serial.println();
    next_scan = millis() + 500;
  }
  delay(1);
}
