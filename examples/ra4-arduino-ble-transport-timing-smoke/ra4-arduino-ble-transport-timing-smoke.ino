#include <ArduinoBLE.h>
#include <utility/HCITransport.h>

namespace {

constexpr unsigned long kSerialReadyTimeoutMs = 2000;
constexpr unsigned long kWaitRequestMs = 250;
constexpr unsigned long kResultIntervalMs = 2000;

bool gBleBegan = false;
unsigned long gBleBeginMs = 0;
unsigned long gTransportWaitMs = 0;
unsigned long gAvailableMs = 0;
int gAvailableResult = -1;
unsigned long gLastResultAt = 0;

void waitForSerial() {
  const unsigned long startedAt = millis();
  while (!Serial && millis() - startedAt < kSerialReadyTimeoutMs) {
  }
}

void printResult() {
  Serial.print("RA4_BLE_TRANSPORT_TIMING:BLE_BEGIN=");
  Serial.println(gBleBegan ? "PASS" : "FAIL");
  Serial.print("RA4_BLE_TRANSPORT_TIMING:BLE_BEGIN_MS=");
  Serial.println(gBleBeginMs);
  Serial.print("RA4_BLE_TRANSPORT_TIMING:WAIT_REQUEST_MS=");
  Serial.println(kWaitRequestMs);
  Serial.print("RA4_BLE_TRANSPORT_TIMING:TRANSPORT_WAIT_MS=");
  Serial.println(gTransportWaitMs);
  Serial.print("RA4_BLE_TRANSPORT_TIMING:AVAILABLE_RESULT=");
  Serial.println(gAvailableResult);
  Serial.print("RA4_BLE_TRANSPORT_TIMING:AVAILABLE_MS=");
  Serial.println(gAvailableMs);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  waitForSerial();

  Serial.println();
  Serial.println("RA4_BLE_TRANSPORT_TIMING:BOOT");

  const unsigned long beginStartedAt = millis();
  gBleBegan = BLE.begin();
  gBleBeginMs = millis() - beginStartedAt;

  if (gBleBegan) {
    const unsigned long waitStartedAt = millis();
    HCITransport.wait(kWaitRequestMs);
    gTransportWaitMs = millis() - waitStartedAt;

    const unsigned long availableStartedAt = millis();
    gAvailableResult = HCITransport.available();
    gAvailableMs = millis() - availableStartedAt;

    BLE.end();
    Serial.println("RA4_BLE_TRANSPORT_TIMING:BLE_END_CALLED");
  }

  printResult();
  Serial.println("RA4_BLE_TRANSPORT_TIMING:DONE");
  gLastResultAt = millis();
}

void loop() {
  if (millis() - gLastResultAt >= kResultIntervalMs) {
    printResult();
    gLastResultAt = millis();
  }
}
