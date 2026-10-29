#include <ArduinoBLE.h>

namespace {

constexpr unsigned long kSerialReadyTimeoutMs = 2000;
constexpr unsigned long kScanTimeoutMs = 20000;
constexpr unsigned long kIndicationTimeoutMs = 8000;
constexpr unsigned long kUnsubscribeSettleMs = 750;
constexpr unsigned long kDisconnectTimeoutMs = 3000;

const char kServiceUuid[] = "9d1f1c20-7df8-4f79-9b7c-5a2b2bc9c021";
const char kCharacteristicUuid[] = "9d1f1c20-7df8-4f79-9b7c-5a2b2bc9c022";
const uint8_t kExpectedIndicationValues[] = {0x19, 0x3b, 0x5d};

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

void finish(const char* failure)
{
  if (gFinished) {
    return;
  }

  if (gScanning) {
    BLE.stopScan();
    gScanning = false;
    Serial.println("RA4_BLE_INDICATE_CENTRAL:SCAN_STOP_CALLED");
  }

  if (gPeripheral && gPeripheral.connected()) {
    gPeripheral.disconnect();
    Serial.println("RA4_BLE_INDICATE_CENTRAL:DISCONNECT_CLEANUP_CALLED");
  }

  if (gBleBegan) {
    BLE.end();
    gBleBegan = false;
    Serial.println("RA4_BLE_INDICATE_CENTRAL:BLE_END_CALLED");
  }

  Serial.println("RA4_BLE_INDICATE_CENTRAL:DONE");
  if (failure == nullptr) {
    Serial.println("RA4_BLE_INDICATE_CENTRAL:RESULT=PASS");
  } else {
    Serial.print("RA4_BLE_INDICATE_CENTRAL:RESULT=FAIL:");
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
  Serial.println("RA4_BLE_INDICATE_CENTRAL:DISCONNECT_CALLED");

  const unsigned long deadline = millis() + kDisconnectTimeoutMs;
  while (gPeripheral.connected() && millis() < deadline) {
    BLE.poll();
  }
  if (gPeripheral.connected()) {
    finish("DISCONNECT_TIMEOUT");
    return;
  }
  Serial.println("RA4_BLE_INDICATE_CENTRAL:DISCONNECT=PASS");
  finish(nullptr);
}

void waitForIndications(BLECharacteristic characteristic)
{
  uint8_t receivedCount = 0;
  const unsigned long deadline = millis() + kIndicationTimeoutMs;
  while (receivedCount < sizeof(kExpectedIndicationValues) &&
         millis() < deadline) {
    BLE.poll();
    if (!characteristic.valueUpdated()) {
      continue;
    }

    if (characteristic.valueLength() != 1 ||
        characteristic.value()[0] != kExpectedIndicationValues[receivedCount]) {
      finish("INDICATION_VALUE");
      return;
    }

    ++receivedCount;
    Serial.print("RA4_BLE_INDICATE_CENTRAL:INDICATION=");
    Serial.print(receivedCount);
    Serial.println(":PASS");
  }

  if (receivedCount != sizeof(kExpectedIndicationValues)) {
    finish("INDICATION_TIMEOUT");
    return;
  }

  if (!characteristic.unsubscribe()) {
    finish("UNSUBSCRIBE");
    return;
  }
  Serial.println("RA4_BLE_INDICATE_CENTRAL:UNSUBSCRIBE=PASS");

  const unsigned long unsubscribeDeadline = millis() + kUnsubscribeSettleMs;
  while (millis() < unsubscribeDeadline) {
    BLE.poll();
  }

  disconnectAndVerify();
}

void runIndicateTransaction(BLEDevice peripheral)
{
  gPeripheral = peripheral;
  Serial.println("RA4_BLE_INDICATE_CENTRAL:TARGET_FOUND");
  BLE.stopScan();
  gScanning = false;
  Serial.println("RA4_BLE_INDICATE_CENTRAL:SCAN_STOP_CALLED");

  if (!gPeripheral.connect()) {
    finish("CONNECT");
    return;
  }
  Serial.println("RA4_BLE_INDICATE_CENTRAL:CONNECT=PASS");

  if (!gPeripheral.discoverAttributes()) {
    finish("DISCOVER");
    return;
  }
  Serial.println("RA4_BLE_INDICATE_CENTRAL:DISCOVER=PASS");

  BLECharacteristic characteristic =
      gPeripheral.characteristic(kCharacteristicUuid);
  if (!characteristic || !characteristic.canSubscribe()) {
    finish("CHARACTERISTIC");
    return;
  }
  Serial.println("RA4_BLE_INDICATE_CENTRAL:CHARACTERISTIC=PASS");

  if (!characteristic.subscribe()) {
    finish("SUBSCRIBE");
    return;
  }
  Serial.println("RA4_BLE_INDICATE_CENTRAL:SUBSCRIBE=PASS");

  waitForIndications(characteristic);
}

}  // namespace

void setup()
{
  Serial.begin(115200);
  waitForSerial();
  Serial.println("RA4_BLE_INDICATE_CENTRAL:BOOT");

  gBleBegan = BLE.begin();
  Serial.println(
      gBleBegan
          ? "RA4_BLE_INDICATE_CENTRAL:BLE_BEGIN=PASS"
          : "RA4_BLE_INDICATE_CENTRAL:BLE_BEGIN=FAIL");
  if (!gBleBegan) {
    finish("BLE_BEGIN");
    return;
  }

  gScanning = BLE.scanForUuid(kServiceUuid) != 0;
  Serial.println(
      gScanning
          ? "RA4_BLE_INDICATE_CENTRAL:SCAN_START=PASS"
          : "RA4_BLE_INDICATE_CENTRAL:SCAN_START=FAIL");
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
    runIndicateTransaction(peripheral);
    return;
  }

  static const unsigned long scanStartedAt = millis();
  if (millis() - scanStartedAt >= kScanTimeoutMs) {
    finish("SCAN_TIMEOUT");
  }
}
