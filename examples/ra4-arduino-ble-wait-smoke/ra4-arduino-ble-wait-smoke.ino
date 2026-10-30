#include <ArduinoBLE.h>

namespace {

constexpr unsigned long kSerialReadyTimeoutMs = 2000;
constexpr unsigned long kWaitTimeoutMs = 250;
constexpr unsigned long kWaitMaximumElapsedMs = 2000;
constexpr unsigned long kResultIntervalMs = 2000;

bool gBleBegan = false;
bool gWaitCompleted = false;
bool gWaitWithinBound = false;
unsigned long gBleBeginMs = 0;
unsigned long gWaitElapsedMs = 0;
unsigned long gLastResultAt = 0;

void waitForSerial() {
  const unsigned long startedAt = millis();
  while (!Serial && millis() - startedAt < kSerialReadyTimeoutMs) {
  }
}

void printResult() {
  Serial.print("RA4_BLE_WAIT_SMOKE:RESULT=");
  Serial.println(gBleBegan && gWaitCompleted && gWaitWithinBound ? "PASS" : "FAIL");
  Serial.print("RA4_BLE_WAIT_SMOKE:BLE_BEGIN_MS=");
  Serial.println(gBleBeginMs);
  Serial.print("RA4_BLE_WAIT_SMOKE:WAIT_REQUEST_MS=");
  Serial.println(kWaitTimeoutMs);
  Serial.print("RA4_BLE_WAIT_SMOKE:WAIT_MAX_MS=");
  Serial.println(kWaitMaximumElapsedMs);
  Serial.print("RA4_BLE_WAIT_SMOKE:WAIT_ELAPSED_MS=");
  Serial.println(gWaitElapsedMs);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  waitForSerial();

  Serial.println();
  Serial.println("RA4_BLE_WAIT_SMOKE:BOOT");

  const unsigned long beginStartedAt = millis();
  gBleBegan = BLE.begin();
  gBleBeginMs = millis() - beginStartedAt;

  if (gBleBegan) {
    const unsigned long waitStartedAt = millis();
    BLE.poll(kWaitTimeoutMs);
    gWaitElapsedMs = millis() - waitStartedAt;
    gWaitCompleted = true;
    gWaitWithinBound = gWaitElapsedMs <= kWaitMaximumElapsedMs;
    BLE.end();
    Serial.println("RA4_BLE_WAIT_SMOKE:BLE_END_CALLED");
  }

  printResult();
  Serial.println("RA4_BLE_WAIT_SMOKE:DONE");
  gLastResultAt = millis();
}

void loop() {
  if (millis() - gLastResultAt >= kResultIntervalMs) {
    printResult();
    gLastResultAt = millis();
  }
}
