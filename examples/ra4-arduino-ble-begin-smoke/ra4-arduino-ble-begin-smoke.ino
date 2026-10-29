#include <ArduinoBLE.h>

namespace {

constexpr unsigned long kSerialReadyTimeoutMs = 2000;
constexpr unsigned long kResultIntervalMs = 2000;

bool gBleBegan = false;
unsigned long gBleBeginMs = 0;
unsigned long gLastResultAt = 0;

void waitForSerial() {
  const unsigned long startedAt = millis();
  while (!Serial && millis() - startedAt < kSerialReadyTimeoutMs) {
  }
}

void printResult(const char* marker) {
  Serial.print("RA4_BLE_SMOKE:");
  Serial.print(marker);
  Serial.print("=");
  Serial.println(gBleBegan ? "PASS" : "FAIL");
  Serial.print("RA4_BLE_SMOKE:BLE_BEGIN_MS=");
  Serial.println(gBleBeginMs);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  waitForSerial();

  Serial.println();
  Serial.println("RA4_BLE_SMOKE:BOOT");

  const unsigned long startedAt = millis();
  gBleBegan = BLE.begin();
  gBleBeginMs = millis() - startedAt;
  printResult("BLE_BEGIN");

  if (gBleBegan) {
    BLE.end();
    Serial.println("RA4_BLE_SMOKE:BLE_END_CALLED");
  }

  Serial.println("RA4_BLE_SMOKE:DONE");
  gLastResultAt = millis();
}

void loop() {
  if (millis() - gLastResultAt >= kResultIntervalMs) {
    printResult("RESULT");
    gLastResultAt = millis();
  }
}
