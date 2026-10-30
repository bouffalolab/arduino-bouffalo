#include <ArduinoBLE.h>

namespace {

constexpr unsigned long kSerialReadyTimeoutMs = 2000;
constexpr unsigned long kInitialNotifyDelayMs = 500;
constexpr unsigned long kNotifyIntervalMs = 300;

const char kServiceUuid[] = "9d1f1c20-7df8-4f79-9b7c-5a2b2bc9c011";
const char kCharacteristicUuid[] = "9d1f1c20-7df8-4f79-9b7c-5a2b2bc9c012";
const uint8_t kNotificationValues[] = {0x31, 0x52, 0x73};

BLEService gService(kServiceUuid);
BLEByteCharacteristic gCharacteristic(
    kCharacteristicUuid,
    BLERead | BLENotify);

bool gConnected = false;
bool gSubscribed = false;
uint8_t gNotificationIndex = 0;
unsigned long gNextNotificationAt = 0;
unsigned long gLastReadyAt = 0;

void waitForSerial()
{
  const unsigned long startedAt = millis();
  while (!Serial && millis() - startedAt < kSerialReadyTimeoutMs) {
  }
}

void printFailure(const char* reason)
{
  Serial.print("RA4_BLE_NOTIFY_PERIPHERAL:RESULT=FAIL:");
  Serial.println(reason);
}

}  // namespace

void setup()
{
  Serial.begin(115200);
  waitForSerial();
  Serial.println("RA4_BLE_NOTIFY_PERIPHERAL:BOOT");

  if (!BLE.begin()) {
    Serial.println("RA4_BLE_NOTIFY_PERIPHERAL:BLE_BEGIN=FAIL");
    printFailure("BLE_BEGIN");
    return;
  }
  Serial.println("RA4_BLE_NOTIFY_PERIPHERAL:BLE_BEGIN=PASS");

  BLE.setLocalName("R4NotifyGate");
  BLE.setAdvertisedService(gService);
  gService.addCharacteristic(gCharacteristic);
  BLE.addService(gService);
  gCharacteristic.writeValue(kNotificationValues[0]);

  if (!BLE.advertise()) {
    Serial.println("RA4_BLE_NOTIFY_PERIPHERAL:ADVERTISE=FAIL");
    BLE.end();
    printFailure("ADVERTISE");
    return;
  }

  Serial.println("RA4_BLE_NOTIFY_PERIPHERAL:READY");
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
            ? "RA4_BLE_NOTIFY_PERIPHERAL:CENTRAL_CONNECTED"
            : "RA4_BLE_NOTIFY_PERIPHERAL:CENTRAL_DISCONNECTED");
  }

  const bool subscribed = gCharacteristic.subscribed();
  if (subscribed && !gSubscribed) {
    gSubscribed = true;
    gNotificationIndex = 0;
    gNextNotificationAt = millis() + kInitialNotifyDelayMs;
    Serial.println("RA4_BLE_NOTIFY_PERIPHERAL:SUBSCRIBED");
  } else if (!subscribed && gSubscribed) {
    gSubscribed = false;
    gNextNotificationAt = 0;
    Serial.println("RA4_BLE_NOTIFY_PERIPHERAL:SUBSCRIPTION_CLEARED");
  }

  if (gSubscribed && gNotificationIndex < sizeof(kNotificationValues) &&
      millis() >= gNextNotificationAt) {
    if (!gCharacteristic.writeValue(kNotificationValues[gNotificationIndex])) {
      printFailure("NOTIFY_SEND");
      return;
    }

    ++gNotificationIndex;
    Serial.print("RA4_BLE_NOTIFY_PERIPHERAL:NOTIFY_SENT=");
    Serial.println(gNotificationIndex);
    gNextNotificationAt = millis() + kNotifyIntervalMs;
  }

  if (!gConnected && millis() - gLastReadyAt >= 1000) {
    Serial.println("RA4_BLE_NOTIFY_PERIPHERAL:READY");
    gLastReadyAt = millis();
  }
}
