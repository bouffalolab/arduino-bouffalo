#include <ArduinoBLE.h>
#include <utility/HCI.h>

namespace {

constexpr unsigned long kSerialReadyTimeoutMs = 2000;
constexpr unsigned long kResultIntervalMs = 2000;

bool gFinished = false;
bool gPass = false;
unsigned long gLastResultAt = 0;

void waitForSerial()
{
  const unsigned long startedAt = millis();
  while (!Serial && millis() - startedAt < kSerialReadyTimeoutMs) {
  }
}

void report(const char* step, int status)
{
  Serial.print("RA4_BLE_INIT:");
  Serial.print(step);
  Serial.print("=");
  Serial.println(status == 0 ? "PASS" : "FAIL");
  Serial.print("RA4_BLE_INIT:");
  Serial.print(step);
  Serial.print("_STATUS=");
  Serial.println(status);
}

void finish()
{
  gFinished = true;
  gLastResultAt = millis();
  Serial.print("RA4_BLE_INIT:RESULT=");
  Serial.println(gPass ? "PASS" : "FAIL");
}

}  // namespace

void setup()
{
  Serial.begin(115200);
  waitForSerial();
  Serial.println("RA4_BLE_INIT:BOOT");

  const int begin_status = BLE.begin() ? 0 : -1;
  report("BLE_BEGIN", begin_status);
  if (begin_status != 0) {
    finish();
    return;
  }

  int status = HCI.leSetScanEnable(false, true);
  report("SCAN_DISABLE_IDLE", status);
  if (status == 0) {
    status = HCI.leSetScanParameters(0x01, 0x0020, 0x0020, 0x00, 0x00);
    report("SCAN_PARAMETERS", status);
  }
  if (status == 0) {
    status = HCI.leSetScanEnable(true, true);
    report("SCAN_ENABLE", status);
  }
  if (status == 0) {
    delay(250);
    status = HCI.leSetScanEnable(false, false);
    report("SCAN_DISABLE_ACTIVE", status);
  }
  BLE.end();
  Serial.println("RA4_BLE_INIT:BLE_END_CALLED");

  gPass = status == 0;
  finish();
}

void loop()
{
  if (!gFinished) {
    return;
  }
  if (millis() - gLastResultAt >= kResultIntervalMs) {
    Serial.print("RA4_BLE_INIT:RESULT=");
    Serial.println(gPass ? "PASS" : "FAIL");
    gLastResultAt = millis();
  }
}
