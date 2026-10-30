#include <ArduinoBLE.h>
#include <WiFiS3.h>
#include <stdlib.h>
#include <string.h>

// Credentials arrive only over USB at runtime. IPv6 uses the BL diagnostic
// socket, while RA4 continuously polls BLE and answers numbered CDC echoes.
namespace {
char command[128] = {};
size_t used = 0;
bool ap_requested = false, active = false, control_only = false, finished = false;
unsigned long next_ready = 0, started = 0;
unsigned long reports = 0;
void fail(const char *reason) {
  if (active) { BLE.stopScan(); BLE.end(); }
  active = false;
  finished = true;
  Serial.print("V6_COEX:FAIL="); Serial.println(reason);
}
bool start_scan() {
  active = BLE.begin();
  if (!active || !BLE.setScanParameters(32, 32) || !BLE.scan(true)) {
    fail("BLE"); return false;
  }
  started = millis();
  Serial.println("V6_COEX:ACTIVE_SCAN=32,32");
  return true;
}
void dispatch() {
  if (strcmp(command, "PRESCAN") == 0 && !ap_requested && !active) {
    start_scan();
  } else if (strncmp(command, "START ", 6) == 0 && !ap_requested) {
    char *ssid = command + 6, *password = strchr(ssid, ' ');
    if (!password) { fail("COMMAND"); return; }
    *password++ = 0;
    if (WiFi.beginAP(ssid, password, 6) != WL_AP_LISTENING) { fail("AP"); return; }
    ap_requested = true;
    started = millis();
    Serial.println("V6_COEX:AP_REQUESTED");
  } else if (strcmp(command, "SCAN") == 0 && ap_requested && !active) {
    start_scan();
  } else if (strcmp(command, "CONTROL") == 0 && ap_requested && !active) {
    control_only = true;
    Serial.println("V6_COEX:CONTROL_WITHOUT_BLE");
  } else if (strcmp(command, "SNAP") == 0 && (active || control_only)) {
    Serial.print("V6_COEX:BLE_REPORTS="); Serial.println(reports);
  } else if (strncmp(command, "E ", 2) == 0 && (active || control_only)) {
    char *end = nullptr;
    unsigned long sequence = strtoul(command + 2, &end, 10);
    if (!sequence || *end) { fail("ECHO_COMMAND"); return; }
    Serial.print("V6_COEX:USB_ECHO="); Serial.println(sequence);
  } else if (strcmp(command, "STOP") == 0 && ap_requested) {
    if (active) { BLE.stopScan(); BLE.end(); active = false; }
    Serial.print("V6_COEX:BLE_FINAL="); Serial.println(reports);
    Serial.println("V6_COEX:STOPPED"); finished = true;
  } else { fail("COMMAND"); }
}
}
void setup() { Serial.begin(115200); }
void loop() {
  if (finished) { delay(10); return; }
  if (active) {
    if (BLE.available()) ++reports;
  }
  if (ap_requested && millis() - started > 180000UL) { fail("HOST_DEADLINE"); return; }
  while (Serial.available()) {
    int c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      command[used] = 0; used = 0; dispatch();
      if (finished) return;
    } else if (used + 1 < sizeof(command)) command[used++] = static_cast<char>(c);
    else { fail("COMMAND_LENGTH"); return; }
  }
  if (!ap_requested && static_cast<long>(millis() - next_ready) >= 0) {
    Serial.println("V6_COEX:READY"); next_ready = millis() + 1000;
  }
  delay(5);
}
