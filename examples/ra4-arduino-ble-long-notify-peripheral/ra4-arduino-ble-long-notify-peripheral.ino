#include <ArduinoBLE.h>

namespace {

constexpr unsigned long kSerialReadyTimeoutMs = 2000;
constexpr unsigned long kInitialNotifyDelayMs = 500;
constexpr unsigned long kNotifyIntervalMs = 300;
constexpr uint8_t kNotificationCount = 3;
constexpr size_t kPayloadBytes = 60;

const char kServiceUuid[] = "9d1f1c20-7df8-4f79-9b7c-5a2b2bc9c031";
const char kCharacteristicUuid[] = "9d1f1c20-7df8-4f79-9b7c-5a2b2bc9c032";

BLEService gService(kServiceUuid);
BLECharacteristic gCharacteristic(
    kCharacteristicUuid,
    BLERead | BLENotify,
    kPayloadBytes,
    true);

bool gConnected = false;
bool gSubscribed = false;
uint8_t gNotificationIndex = 0;
unsigned long gNextNotificationAt = 0;
unsigned long gLastReadyAt = 0;
uint8_t gPayload[kPayloadBytes] = {};

void waitForSerial()
{
  const unsigned long startedAt = millis();
  while (!Serial && millis() - startedAt < kSerialReadyTimeoutMs) {
  }
}

uint8_t payloadByte(uint8_t sequence, size_t offset)
{
  return static_cast<uint8_t>(
      0x2dU + static_cast<uint8_t>(sequence * 0x3bU) +
      static_cast<uint8_t>(offset * 0x11U));
}

uint32_t payloadChecksum(const uint8_t* value, size_t length)
{
  uint32_t checksum = 2166136261UL;
  for (size_t i = 0; i < length; ++i) {
    checksum ^= value[i];
    checksum *= 16777619UL;
  }
  return checksum;
}

void preparePayload(uint8_t sequence)
{
  for (size_t i = 0; i < sizeof(gPayload); ++i) {
    gPayload[i] = payloadByte(sequence, i);
  }
}

void printFailure(const char* reason)
{
  Serial.print("RA4_BLE_LONG_NOTIFY_PERIPHERAL:RESULT=FAIL:");
  Serial.println(reason);
}

}  // namespace

void setup()
{
  Serial.begin(115200);
  waitForSerial();
  Serial.println("RA4_BLE_LONG_NOTIFY_PERIPHERAL:BOOT");

  if (!BLE.begin()) {
    Serial.println("RA4_BLE_LONG_NOTIFY_PERIPHERAL:BLE_BEGIN=FAIL");
    printFailure("BLE_BEGIN");
    return;
  }
  Serial.println("RA4_BLE_LONG_NOTIFY_PERIPHERAL:BLE_BEGIN=PASS");

  BLE.setLocalName("R4LongNotifyGate");
  BLE.setAdvertisedService(gService);
  gService.addCharacteristic(gCharacteristic);
  BLE.addService(gService);
  preparePayload(0);
  gCharacteristic.writeValue(gPayload, sizeof(gPayload));

  if (!BLE.advertise()) {
    Serial.println("RA4_BLE_LONG_NOTIFY_PERIPHERAL:ADVERTISE=FAIL");
    BLE.end();
    printFailure("ADVERTISE");
    return;
  }

  Serial.println("RA4_BLE_LONG_NOTIFY_PERIPHERAL:READY");
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
            ? "RA4_BLE_LONG_NOTIFY_PERIPHERAL:CENTRAL_CONNECTED"
            : "RA4_BLE_LONG_NOTIFY_PERIPHERAL:CENTRAL_DISCONNECTED");
  }

  const bool subscribed = gCharacteristic.subscribed();
  if (subscribed && !gSubscribed) {
    gSubscribed = true;
    gNotificationIndex = 0;
    gNextNotificationAt = millis() + kInitialNotifyDelayMs;
    Serial.println("RA4_BLE_LONG_NOTIFY_PERIPHERAL:SUBSCRIBED");
  } else if (!subscribed && gSubscribed) {
    gSubscribed = false;
    gNextNotificationAt = 0;
    Serial.println("RA4_BLE_LONG_NOTIFY_PERIPHERAL:SUBSCRIPTION_CLEARED");
  }

  if (gSubscribed && gNotificationIndex < kNotificationCount &&
      millis() >= gNextNotificationAt) {
    preparePayload(gNotificationIndex);
    if (!gCharacteristic.writeValue(gPayload, sizeof(gPayload))) {
      printFailure("NOTIFY_SEND");
      return;
    }

    ++gNotificationIndex;
    Serial.print("RA4_BLE_LONG_NOTIFY_PERIPHERAL:NOTIFY_SENT=");
    Serial.print(gNotificationIndex);
    Serial.print(":LENGTH=");
    Serial.print(sizeof(gPayload));
    Serial.print(":CHECKSUM=");
    Serial.println(payloadChecksum(gPayload, sizeof(gPayload)));
    gNextNotificationAt = millis() + kNotifyIntervalMs;
  }

  if (!gConnected && millis() - gLastReadyAt >= 1000) {
    Serial.println("RA4_BLE_LONG_NOTIFY_PERIPHERAL:READY");
    gLastReadyAt = millis();
  }
}
