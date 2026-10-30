#include <ArduinoBLE.h>

namespace {

constexpr unsigned long kSerialReadyTimeoutMs = 2000;
constexpr unsigned long kScanTimeoutMs = 20000;
constexpr unsigned long kNotificationTimeoutMs = 8000;
constexpr unsigned long kUnsubscribeSettleMs = 750;
constexpr unsigned long kDisconnectTimeoutMs = 3000;
constexpr uint8_t kNotificationCount = 3;
constexpr size_t kPayloadBytes = 60;

const char kServiceUuid[] = "9d1f1c20-7df8-4f79-9b7c-5a2b2bc9c031";
const char kCharacteristicUuid[] = "9d1f1c20-7df8-4f79-9b7c-5a2b2bc9c032";

bool gBleBegan = false;
bool gScanning = false;
bool gFinished = false;
BLEDevice gPeripheral;

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

bool validatePayload(const BLECharacteristic& characteristic, uint8_t sequence)
{
  if (characteristic.valueLength() != static_cast<int>(kPayloadBytes) ||
      characteristic.value() == nullptr) {
    return false;
  }

  const uint8_t* value = characteristic.value();
  for (size_t i = 0; i < kPayloadBytes; ++i) {
    if (value[i] != payloadByte(sequence, i)) {
      return false;
    }
  }
  return true;
}

void finish(const char* failure)
{
  if (gFinished) {
    return;
  }

  if (gScanning) {
    BLE.stopScan();
    gScanning = false;
    Serial.println("RA4_BLE_LONG_NOTIFY_CENTRAL:SCAN_STOP_CALLED");
  }

  if (gPeripheral && gPeripheral.connected()) {
    gPeripheral.disconnect();
    Serial.println("RA4_BLE_LONG_NOTIFY_CENTRAL:DISCONNECT_CLEANUP_CALLED");
  }

  if (gBleBegan) {
    BLE.end();
    gBleBegan = false;
    Serial.println("RA4_BLE_LONG_NOTIFY_CENTRAL:BLE_END_CALLED");
  }

  Serial.println("RA4_BLE_LONG_NOTIFY_CENTRAL:DONE");
  if (failure == nullptr) {
    Serial.println("RA4_BLE_LONG_NOTIFY_CENTRAL:RESULT=PASS");
  } else {
    Serial.print("RA4_BLE_LONG_NOTIFY_CENTRAL:RESULT=FAIL:");
    Serial.println(failure);
  }
  gFinished = true;
}

void disconnectAndVerify()
{
  if (!gPeripheral.disconnect()) {
    finish("DISCONNECT_REQUEST");
    return;
  }
  Serial.println("RA4_BLE_LONG_NOTIFY_CENTRAL:DISCONNECT_CALLED");

  const unsigned long deadline = millis() + kDisconnectTimeoutMs;
  while (gPeripheral.connected() && millis() < deadline) {
    BLE.poll();
  }
  if (gPeripheral.connected()) {
    finish("DISCONNECT_TIMEOUT");
    return;
  }
  Serial.println("RA4_BLE_LONG_NOTIFY_CENTRAL:DISCONNECT=PASS");
  finish(nullptr);
}

void waitForNotifications(BLECharacteristic characteristic)
{
  uint8_t receivedCount = 0;
  const unsigned long deadline = millis() + kNotificationTimeoutMs;
  while (receivedCount < kNotificationCount && millis() < deadline) {
    BLE.poll();
    if (!characteristic.valueUpdated()) {
      continue;
    }

    if (!validatePayload(characteristic, receivedCount)) {
      finish("NOTIFICATION_VALUE");
      return;
    }

    const uint8_t* value = characteristic.value();
    ++receivedCount;
    Serial.print("RA4_BLE_LONG_NOTIFY_CENTRAL:NOTIFICATION=");
    Serial.print(receivedCount);
    Serial.print(":LENGTH=");
    Serial.print(characteristic.valueLength());
    Serial.print(":CHECKSUM=");
    Serial.println(payloadChecksum(value, kPayloadBytes));
  }

  if (receivedCount != kNotificationCount) {
    finish("NOTIFICATION_TIMEOUT");
    return;
  }

  if (!characteristic.unsubscribe()) {
    finish("UNSUBSCRIBE");
    return;
  }
  Serial.println("RA4_BLE_LONG_NOTIFY_CENTRAL:UNSUBSCRIBE=PASS");

  const unsigned long unsubscribeDeadline = millis() + kUnsubscribeSettleMs;
  while (millis() < unsubscribeDeadline) {
    BLE.poll();
  }

  disconnectAndVerify();
}

void runNotifyTransaction(BLEDevice peripheral)
{
  gPeripheral = peripheral;
  Serial.println("RA4_BLE_LONG_NOTIFY_CENTRAL:TARGET_FOUND");
  BLE.stopScan();
  gScanning = false;
  Serial.println("RA4_BLE_LONG_NOTIFY_CENTRAL:SCAN_STOP_CALLED");

  if (!gPeripheral.connect()) {
    finish("CONNECT");
    return;
  }
  Serial.println("RA4_BLE_LONG_NOTIFY_CENTRAL:CONNECT=PASS");

  if (!gPeripheral.discoverAttributes()) {
    finish("DISCOVER");
    return;
  }
  Serial.println("RA4_BLE_LONG_NOTIFY_CENTRAL:DISCOVER=PASS");

  BLECharacteristic characteristic =
      gPeripheral.characteristic(kCharacteristicUuid);
  if (!characteristic || !characteristic.canSubscribe()) {
    finish("CHARACTERISTIC");
    return;
  }
  Serial.println("RA4_BLE_LONG_NOTIFY_CENTRAL:CHARACTERISTIC=PASS");

  if (!characteristic.subscribe()) {
    finish("SUBSCRIBE");
    return;
  }
  Serial.println("RA4_BLE_LONG_NOTIFY_CENTRAL:SUBSCRIBE=PASS");

  waitForNotifications(characteristic);
}

}  // namespace

void setup()
{
  Serial.begin(115200);
  waitForSerial();
  Serial.println("RA4_BLE_LONG_NOTIFY_CENTRAL:BOOT");

  gBleBegan = BLE.begin();
  Serial.println(
      gBleBegan
          ? "RA4_BLE_LONG_NOTIFY_CENTRAL:BLE_BEGIN=PASS"
          : "RA4_BLE_LONG_NOTIFY_CENTRAL:BLE_BEGIN=FAIL");
  if (!gBleBegan) {
    finish("BLE_BEGIN");
    return;
  }

  gScanning = BLE.scanForUuid(kServiceUuid) != 0;
  Serial.println(
      gScanning
          ? "RA4_BLE_LONG_NOTIFY_CENTRAL:SCAN_START=PASS"
          : "RA4_BLE_LONG_NOTIFY_CENTRAL:SCAN_START=FAIL");
  if (!gScanning) {
    finish("SCAN_START");
  }
}

void loop()
{
  if (gFinished) {
    return;
  }

  BLEDevice peripheral = BLE.available();
  if (peripheral) {
    runNotifyTransaction(peripheral);
    return;
  }

  static const unsigned long scanStartedAt = millis();
  if (millis() - scanStartedAt >= kScanTimeoutMs) {
    finish("SCAN_TIMEOUT");
  }
}
