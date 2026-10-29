#include <ArduinoBLE.h>

namespace {

constexpr unsigned long kSerialReadyTimeoutMs = 2000;
constexpr unsigned long kScanDurationMs = 10000;
constexpr unsigned long kResultIntervalMs = 2000;

bool gBleBegan = false;
bool gScanStarted = false;
bool gFinished = false;
unsigned long gScanStartedAt = 0;
unsigned long gLastResultAt = 0;
unsigned long gReportCount = 0;
const char* gFailure = nullptr;

void waitForSerial()
{
  const unsigned long startedAt = millis();
  while (!Serial && millis() - startedAt < kSerialReadyTimeoutMs) {
  }
}

void printResult()
{
  Serial.print("RA4_BLE_SCAN:REPORT_COUNT=");
  Serial.println(gReportCount);
  Serial.print("RA4_BLE_SCAN:REPORT_OBSERVED=");
  Serial.println(gReportCount > 0 ? "YES" : "NO");
  if (gFailure == nullptr) {
    Serial.println("RA4_BLE_SCAN:RESULT=PASS");
  } else {
    Serial.print("RA4_BLE_SCAN:RESULT=FAIL:");
    Serial.println(gFailure);
  }
}

void finishScan()
{
  if (gFinished) {
    return;
  }

  if (gScanStarted) {
    BLE.stopScan();
    Serial.println("RA4_BLE_SCAN:SCAN_STOP_CALLED");
  }
  if (gBleBegan) {
    BLE.end();
    Serial.println("RA4_BLE_SCAN:BLE_END_CALLED");
  }
  Serial.println("RA4_BLE_SCAN:DONE");
  gFinished = true;
  gLastResultAt = millis();
}

}  // namespace

void setup()
{
  Serial.begin(115200);
  waitForSerial();

  Serial.println("RA4_BLE_SCAN:BOOT");

  gBleBegan = BLE.begin();
  Serial.print("RA4_BLE_SCAN:BLE_BEGIN=");
  Serial.println(gBleBegan ? "PASS" : "FAIL");
  if (!gBleBegan) {
    gFailure = "BLE_BEGIN";
    finishScan();
    return;
  }

  gScanStarted = BLE.scan() != 0;
  Serial.print("RA4_BLE_SCAN:SCAN_START=");
  Serial.println(gScanStarted ? "PASS" : "FAIL");
  if (!gScanStarted) {
    gFailure = "SCAN_START";
    finishScan();
    return;
  }

  gScanStartedAt = millis();
}

void loop()
{
  if (!gFinished) {
    BLEDevice peripheral = BLE.available();
    if (peripheral) {
      ++gReportCount;
    }

    if (millis() - gScanStartedAt >= kScanDurationMs) {
      finishScan();
    }
    return;
  }

  if (millis() - gLastResultAt >= kResultIntervalMs) {
    printResult();
    gLastResultAt = millis();
  }
}
