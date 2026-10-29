#include <ArduinoBLE.h>

namespace {

constexpr unsigned long kSerialReadyTimeoutMs = 2000;
constexpr unsigned long kResultIntervalMs = 2000;

bool gBleBegan = false;
unsigned long gLastResultAt = 0;

void waitForSerial()
{
  const unsigned long startedAt = millis();
  while (!Serial && millis() - startedAt < kSerialReadyTimeoutMs) {
  }
}

void printResult()
{
  Serial.print("RA4_BLE_BEGIN_TRACE:RESULT=");
  Serial.println(gBleBegan ? "PASS" : "FAIL");
}

}  // namespace

void setup()
{
  Serial.begin(115200);
  waitForSerial();

  Serial.println("RA4_BLE_BEGIN_TRACE:BOOT");
  BLE.debug(Serial);
  gBleBegan = BLE.begin();
  Serial.print("RA4_BLE_BEGIN_TRACE:BLE_BEGIN=");
  Serial.println(gBleBegan ? "PASS" : "FAIL");

  if (gBleBegan) {
    BLE.end();
    Serial.println("RA4_BLE_BEGIN_TRACE:BLE_END_CALLED");
  }

  Serial.println("RA4_BLE_BEGIN_TRACE:DONE");
  gLastResultAt = millis();
}

void loop()
{
  if (millis() - gLastResultAt >= kResultIntervalMs) {
    printResult();
    gLastResultAt = millis();
  }
}
