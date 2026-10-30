#include <ArduinoBLE.h>

namespace {

constexpr unsigned long kSerialReadyTimeoutMs = 2000;
constexpr unsigned long kInitialIndicationDelayMs = 500;
constexpr unsigned long kIndicationIntervalMs = 300;

const char kServiceUuid[] = "9d1f1c20-7df8-4f79-9b7c-5a2b2bc9c021";
const char kCharacteristicUuid[] = "9d1f1c20-7df8-4f79-9b7c-5a2b2bc9c022";
const uint8_t kIndicationValues[] = {0x19, 0x3b, 0x5d};

BLEService gService(kServiceUuid);
BLEByteCharacteristic gCharacteristic(
    kCharacteristicUuid,
    BLERead | BLEIndicate);

bool gConnected = false;
bool gSubscribed = false;
uint8_t gIndicationIndex = 0;
unsigned long gNextIndicationAt = 0;
unsigned long gLastReadyAt = 0;

void waitForSerial()
{
  const unsigned long startedAt = millis();
  while (!Serial && millis() - startedAt < kSerialReadyTimeoutMs) {
  }
}

void printFailure(const char* reason)
{
  Serial.print("RA4_BLE_INDICATE_PERIPHERAL:RESULT=FAIL:");
  Serial.println(reason);
}

}  // namespace

void setup()
{
  Serial.begin(115200);
  waitForSerial();
  Serial.println("RA4_BLE_INDICATE_PERIPHERAL:BOOT");

  if (!BLE.begin()) {
    Serial.println("RA4_BLE_INDICATE_PERIPHERAL:BLE_BEGIN=FAIL");
    printFailure("BLE_BEGIN");
    return;
  }
  Serial.println("RA4_BLE_INDICATE_PERIPHERAL:BLE_BEGIN=PASS");

  BLE.setLocalName("R4IndicateGate");
  BLE.setAdvertisedService(gService);
  gService.addCharacteristic(gCharacteristic);
  BLE.addService(gService);
  gCharacteristic.writeValue(kIndicationValues[0]);

  if (!BLE.advertise()) {
    Serial.println("RA4_BLE_INDICATE_PERIPHERAL:ADVERTISE=FAIL");
    BLE.end();
    printFailure("ADVERTISE");
    return;
  }

  Serial.println("RA4_BLE_INDICATE_PERIPHERAL:READY");
  gLastReadyAt = millis();
}

void loop()
{
  BLE.poll();

  const bool connected = BLE.connected();
  if (connected != gConnected) {
    gConnected = connected;
    Serial.println(
        connected
            ? "RA4_BLE_INDICATE_PERIPHERAL:CENTRAL_CONNECTED"
            : "RA4_BLE_INDICATE_PERIPHERAL:CENTRAL_DISCONNECTED");
  }

  const bool subscribed = gCharacteristic.subscribed();
  if (subscribed && !gSubscribed) {
    gSubscribed = true;
    gIndicationIndex = 0;
    gNextIndicationAt = millis() + kInitialIndicationDelayMs;
    Serial.println("RA4_BLE_INDICATE_PERIPHERAL:SUBSCRIBED");
  } else if (!subscribed && gSubscribed) {
    gSubscribed = false;
    gNextIndicationAt = 0;
    Serial.println("RA4_BLE_INDICATE_PERIPHERAL:SUBSCRIPTION_CLEARED");
  }

  if (gSubscribed && gIndicationIndex < sizeof(kIndicationValues) &&
      millis() >= gNextIndicationAt) {
    if (!gCharacteristic.writeValue(kIndicationValues[gIndicationIndex])) {
      printFailure("INDICATION_SEND");
      return;
    }

    ++gIndicationIndex;
    Serial.print("RA4_BLE_INDICATE_PERIPHERAL:INDICATION_CONFIRMED=");
    Serial.println(gIndicationIndex);
    gNextIndicationAt = millis() + kIndicationIntervalMs;
  }

  if (!gConnected && millis() - gLastReadyAt >= 1000) {
    Serial.println("RA4_BLE_INDICATE_PERIPHERAL:READY");
    gLastReadyAt = millis();
  }
}
