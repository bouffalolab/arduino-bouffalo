#include <ArduinoBLE.h>
#include <Modem.h>
#include <WiFiS3.h>

namespace {
#ifndef LOCAL_COEX_CYCLES
#define LOCAL_COEX_CYCLES 40
#endif
constexpr unsigned kCycles = LOCAL_COEX_CYCLES;
bool started = false;
bool finished = false;
bool ble_active = false;
int slot = -1;

bool heap_sample(unsigned cycle) {
  std::string response;
  modem.begin();
  if (!modem.write("+SSLCLIENTDIAG:", response,
                   "AT+SSLCLIENTDIAG=%d\r\n", slot)) return false;
  unsigned long fields[15] = {};
  const char *cursor = response.c_str();
  for (unsigned index = 0; index < 15; ++index) {
    char *end = nullptr;
    fields[index] = strtoul(cursor, &end, 10);
    if (end == cursor) return false;
    if (index < 14 && *end != ',') return false;
    cursor = end + (index < 14 ? 1 : 0);
  }
  Serial.print("LOCAL_COEX:HEAP=");
  Serial.print(cycle);
  Serial.print(',');
  Serial.print(fields[13]);
  Serial.print(',');
  Serial.println(fields[14]);
  return true;
}

void finish(bool success) {
  if (ble_active) { BLE.stopScan(); BLE.end(); ble_active = false; }
  if (slot >= 2000) {
    std::string response;
    modem.begin();
    if (!modem.write("+SSLCLIENTCLOSE:", response,
                     "AT+SSLCLIENTCLOSE=%d\r\n", slot)) success = false;
  }
  Serial.println(success ? "LOCAL_COEX:RESULT=PASS" : "LOCAL_COEX:RESULT=FAIL");
  finished = true;
}

void run_gate() {
  std::string response;
  modem.begin();
  if (!modem.write("+SSLBEGINCLIENT:", response, "AT+SSLBEGINCLIENT\r\n")) {
    finish(false);
    return;
  }
  slot = atoi(response.c_str());
  if (slot < 2000 || slot > 2007 || !heap_sample(0)) {
    finish(false);
    return;
  }
  const unsigned long started_at = millis();
  for (unsigned cycle = 1; cycle <= kCycles; ++cycle) {
    ble_active = BLE.begin();
    if (!ble_active || !BLE.scan()) { finish(false); return; }
    unsigned reports = 0;
    unsigned long observe = millis();
    while (millis() - observe < 2000) {
      if (BLE.available()) ++reports;
      delay(5);
    }
    // Scan Wi-Fi while the controller keeps BLE scanning active.
    const int networks = WiFi.scanNetworks();
    observe = millis();
    while (millis() - observe < 2000) {
      if (BLE.available()) ++reports;
      delay(5);
    }
    BLE.stopScan();
    BLE.end();
    ble_active = false;
    if (networks < 0 || !heap_sample(cycle)) { finish(false); return; }
    Serial.println("LOCAL_COEX:USB_ECHO_REQUEST");
    observe = millis();
    while (!Serial.available() && millis() - observe < 5000) delay(5);
    if (!Serial.available() || Serial.read() != 'E') { finish(false); return; }
    Serial.print("LOCAL_COEX:CYCLE=");
    Serial.print(cycle);
    Serial.print(',');
    Serial.print(networks);
    Serial.print(',');
    Serial.println(reports);
  }
  Serial.print("LOCAL_COEX:ELAPSED_MS=");
  Serial.println(millis() - started_at);
  finish(true);
}
}

void setup() { Serial.begin(115200); }
void loop() {
  if (finished) { delay(10); return; }
  if (!started && Serial.available() && Serial.read() == 'S') {
    started = true;
    run_gate();
  }
  if (!started) { Serial.println("LOCAL_COEX:READY"); delay(1000); }
}
