#include <ArduinoBLE.h>

namespace {

constexpr unsigned long kSerialReadyTimeoutMs = 2000;
constexpr unsigned long kScanTimeoutMs = 20000;
constexpr unsigned long kDisconnectTimeoutMs = 3000;
constexpr uint8_t kInitialValue = 0x2a;
constexpr uint8_t kWriteValue = 0xa5;

const char kServiceUuid[] = "9d1f1c20-7df8-4f79-9b7c-5a2b2bc9c001";
const char kCharacteristicUuid[] = "9d1f1c20-7df8-4f79-9b7c-5a2b2bc9c002";

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
    Serial.println("RA4_BLE_GATT_CENTRAL:SCAN_STOP_CALLED");
  }

  if (gPeripheral && gPeripheral.connected()) {
    gPeripheral.disconnect();
    Serial.println("RA4_BLE_GATT_CENTRAL:DISCONNECT_CLEANUP_CALLED");
  }

  if (gBleBegan) {
    BLE.end();
    gBleBegan = false;
    Serial.println("RA4_BLE_GATT_CENTRAL:BLE_END_CALLED");
  }

  Serial.println("RA4_BLE_GATT_CENTRAL:DONE");
  if (failure == nullptr) {
    Serial.println("RA4_BLE_GATT_CENTRAL:RESULT=PASS");
  } else {
    Serial.print("RA4_BLE_GATT_CENTRAL:RESULT=FAIL:");
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
  Serial.println("RA4_BLE_GATT_CENTRAL:DISCONNECT_CALLED");

  const unsigned long deadline = millis() + kDisconnectTimeoutMs;
  while (gPeripheral.connected() && millis() < deadline) {
    BLE.poll();
  }
  if (gPeripheral.connected()) {
    finish("DISCONNECT_TIMEOUT");
    return;
  }
  Serial.println("RA4_BLE_GATT_CENTRAL:DISCONNECT=PASS");
  finish(nullptr);
}

void runGattTransaction(BLEDevice peripheral)
{
  gPeripheral = peripheral;
  Serial.println("RA4_BLE_GATT_CENTRAL:TARGET_FOUND");
  BLE.stopScan();
  gScanning = false;
  Serial.println("RA4_BLE_GATT_CENTRAL:SCAN_STOP_CALLED");

  if (!gPeripheral.connect()) {
    finish("CONNECT");
    return;
  }
  Serial.println("RA4_BLE_GATT_CENTRAL:CONNECT=PASS");

  if (!gPeripheral.discoverAttributes()) {
    finish("DISCOVER");
    return;
  }
  Serial.println("RA4_BLE_GATT_CENTRAL:DISCOVER=PASS");

  BLECharacteristic characteristic =
      gPeripheral.characteristic(kCharacteristicUuid);
  if (!characteristic || !characteristic.canRead() || !characteristic.canWrite()) {
    finish("CHARACTERISTIC");
    return;
  }
  Serial.println("RA4_BLE_GATT_CENTRAL:CHARACTERISTIC=PASS");

  uint8_t value = 0;
  if (characteristic.readValue(value) != 1 || value != kInitialValue) {
    finish("INITIAL_READ");
    return;
  }
  Serial.println("RA4_BLE_GATT_CENTRAL:INITIAL_READ=PASS");

  if (!characteristic.writeValue(kWriteValue)) {
    finish("WRITE");
    return;
  }
  Serial.println("RA4_BLE_GATT_CENTRAL:WRITE=PASS");

  if (!characteristic.read() || characteristic.valueLength() != 1 ||
      characteristic.value()[0] != kWriteValue) {
    finish("READBACK");
    return;
  }
  Serial.println("RA4_BLE_GATT_CENTRAL:READBACK=PASS");

  disconnectAndVerify();
}

}  // namespace

void setup()
{
  Serial.begin(115200);
  waitForSerial();
  Serial.println("RA4_BLE_GATT_CENTRAL:BOOT");

  gBleBegan = BLE.begin();
  Serial.println(
      gBleBegan
          ? "RA4_BLE_GATT_CENTRAL:BLE_BEGIN=PASS"
          : "RA4_BLE_GATT_CENTRAL:BLE_BEGIN=FAIL");
  if (!gBleBegan) {
    finish("BLE_BEGIN");
    return;
  }

  gScanning = BLE.scanForUuid(kServiceUuid) != 0;
  Serial.println(
      gScanning
          ? "RA4_BLE_GATT_CENTRAL:SCAN_START=PASS"
          : "RA4_BLE_GATT_CENTRAL:SCAN_START=FAIL");
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
    runGattTransaction(peripheral);
    return;
  }

  static const unsigned long scanStartedAt = millis();
  if (millis() - scanStartedAt >= kScanTimeoutMs) {
    finish("SCAN_TIMEOUT");
  }
}
