#include <ArduinoBLE.h>
#if defined(ARDUINO_BLE_COEX_SNAPSHOT)
#include <utility/HCI.h>
#endif

namespace {

constexpr unsigned long kSerialReadyTimeoutMs = 2000;
constexpr unsigned long kScanTimeoutMs = 20000;
constexpr unsigned long kDisconnectTimeoutMs = 3000;
constexpr size_t kValueBytes = 512;
#ifndef LONG_GATT_ITERATIONS
#define LONG_GATT_ITERATIONS 8
#endif
#ifndef LONG_GATT_SESSIONS
#define LONG_GATT_SESSIONS 1
#endif
constexpr unsigned kStressIterations = LONG_GATT_ITERATIONS;
constexpr unsigned kSessions = LONG_GATT_SESSIONS;
static_assert(kStressIterations > 0 && kSessions > 0 &&
              kStressIterations * kSessions <= 200, "bounded GATT reconnect gate");

const char kServiceUuid[] = "9d1f1c20-7df8-4f79-9b7c-5a2b2bc9c041";
const char kCharacteristicUuid[] = "9d1f1c20-7df8-4f79-9b7c-5a2b2bc9c042";

bool gBleBegan = false;
bool gScanning = false;
bool gFinished = false;
uint8_t gGeneration = 0;
unsigned gSessions = 0;
unsigned long gScanStartedAt = 0;
BLEDevice gPeripheral;
uint8_t gExpected[kValueBytes] = {};
uint8_t gWriteValue[kValueBytes] = {};

void waitForSerial()
{
  const unsigned long startedAt = millis();
  while (!Serial && millis() - startedAt < kSerialReadyTimeoutMs) {
  }
}

uint8_t payloadByte(uint8_t generation, size_t offset)
{
  return static_cast<uint8_t>(
      0x31U + static_cast<uint8_t>(generation * 0x47U) +
      static_cast<uint8_t>(offset * 0x13U));
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

void fillPayload(uint8_t* destination, uint8_t generation)
{
  for (size_t i = 0; i < kValueBytes; ++i) {
    destination[i] = payloadByte(generation, i);
  }
}

bool validatePayload(const BLECharacteristic& characteristic, const uint8_t* expected)
{
  if (characteristic.valueLength() != static_cast<int>(kValueBytes) ||
      characteristic.value() == nullptr) {
    return false;
  }
  return memcmp(characteristic.value(), expected, kValueBytes) == 0;
}

void finish(const char* failure)
{
  if (gFinished) {
    return;
  }

#if defined(ARDUINO_BLE_COEX_SNAPSHOT)
  if (failure != nullptr && gBleBegan) HCI.printCoexSnapshot(Serial);
#endif

  if (gScanning) {
    BLE.stopScan();
    gScanning = false;
    Serial.println("RA4_BLE_LONG_GATT_CENTRAL:SCAN_STOP_CALLED");
  }

  if (gPeripheral && gPeripheral.connected()) {
    gPeripheral.disconnect();
    Serial.println("RA4_BLE_LONG_GATT_CENTRAL:DISCONNECT_CLEANUP_CALLED");
  }

  if (gBleBegan) {
    BLE.stopAdvertise();
    BLE.end();
    gBleBegan = false;
    Serial.println("RA4_BLE_LONG_GATT_CENTRAL:BLE_END_CALLED");
  }

  Serial.println("RA4_BLE_LONG_GATT_CENTRAL:DONE");
  if (failure == nullptr) {
    Serial.println("RA4_BLE_LONG_GATT_CENTRAL:RESULT=PASS");
  } else {
    Serial.print("RA4_BLE_LONG_GATT_CENTRAL:RESULT=FAIL:");
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
  Serial.println("RA4_BLE_LONG_GATT_CENTRAL:DISCONNECT_CALLED");

  const unsigned long deadline = millis() + kDisconnectTimeoutMs;
  while (gPeripheral.connected() && millis() < deadline) {
    BLE.poll();
  }
  if (gPeripheral.connected()) {
    finish("DISCONNECT_TIMEOUT");
    return;
  }
  Serial.println("RA4_BLE_LONG_GATT_CENTRAL:DISCONNECT=PASS");
  Serial.print("RA4_BLE_LONG_GATT_CENTRAL:SESSION_PASS=");
  Serial.println(++gSessions);
  if (gSessions < kSessions) {
    gPeripheral = BLEDevice();
    gScanning = BLE.scanForUuid(kServiceUuid) != 0;
    gScanStartedAt = millis();
    if (!gScanning) finish("RECONNECT_SCAN");
    return;
  }
  finish(nullptr);
}

void runGattTransaction(BLEDevice peripheral)
{
  gPeripheral = peripheral;
  Serial.println("RA4_BLE_LONG_GATT_CENTRAL:TARGET_FOUND");
  Serial.print("RA4_BLE_LONG_GATT_CENTRAL:TARGET_RSSI=");
  Serial.println(gPeripheral.rssi());
  BLE.stopScan();
  gScanning = false;
  Serial.println("RA4_BLE_LONG_GATT_CENTRAL:SCAN_STOP_CALLED");

  if (!gPeripheral.connect()) {
    finish("CONNECT");
    return;
  }
  Serial.println("RA4_BLE_LONG_GATT_CENTRAL:CONNECT=PASS");

  if (!gPeripheral.discoverAttributes()) {
    finish("DISCOVER");
    return;
  }
  Serial.println("RA4_BLE_LONG_GATT_CENTRAL:DISCOVER=PASS");
#if defined(LONG_GATT_PEER_GATE)
  BLECharacteristic name = gPeripheral.characteristic("2a00");
  BLECharacteristic appearance = gPeripheral.characteristic("2a01");
  BLECharacteristic changed = gPeripheral.characteristic("2a05");
  if (!name || !name.read() || name.valueLength() != 7 ||
      memcmp(name.value(), "Arduino", 7) != 0 ||
      !appearance || !appearance.read() || appearance.valueLength() != 2 ||
      appearance.value()[0] != 0x34 || appearance.value()[1] != 0x12 ||
      gPeripheral.deviceName() != "Arduino" || gPeripheral.appearance() != 0x1234 ||
      !changed || !changed.canSubscribe() || !changed.subscribe() || !changed.unsubscribe()) {
    finish("GAP_GATT_METADATA");
    return;
  }
  Serial.print("RA4_BLE_LONG_GATT_CENTRAL:METADATA_PASS=");
  Serial.println(gSessions + 1);
#endif

  BLECharacteristic characteristic =
      gPeripheral.characteristic(kCharacteristicUuid);
  if (!characteristic || !characteristic.canRead() || !characteristic.canWrite()) {
    finish("CHARACTERISTIC");
    return;
  }
  Serial.println("RA4_BLE_LONG_GATT_CENTRAL:CHARACTERISTIC=PASS");

#if defined(LONG_GATT_DISCOVERY_PROBE)
  fillPayload(gExpected, gGeneration);
  if (!characteristic.read() || !validatePayload(characteristic, gExpected)) {
    finish("PROBE_READ");
    return;
  }
  Serial.print("RA4_BLE_LONG_GATT_CENTRAL:PROBE_READ_PASS=");
  Serial.println(gGeneration);
  disconnectAndVerify();
  return;
#endif

  for (uint8_t iteration = 0; iteration < kStressIterations; ++iteration) {
    fillPayload(gExpected, gGeneration);
    if (!characteristic.read() || !validatePayload(characteristic, gExpected)) {
      finish("LONG_READ");
      return;
    }

    fillPayload(gWriteValue, static_cast<uint8_t>(gGeneration + 1));
    if (!characteristic.writeValue(gWriteValue, sizeof(gWriteValue))) {
      finish("LONG_WRITE");
      return;
    }

    if (!characteristic.read() || !validatePayload(characteristic, gWriteValue)) {
      finish("LONG_READBACK");
      return;
    }
    Serial.print("RA4_BLE_LONG_GATT_CENTRAL:CYCLE_PASS=");
    Serial.println(++gGeneration);
  }

  Serial.print("RA4_BLE_LONG_GATT_CENTRAL:STRESS_ITERATIONS=");
  Serial.println(kStressIterations);
  Serial.print("RA4_BLE_LONG_GATT_CENTRAL:FINAL_CHECKSUM=");
  Serial.println(payloadChecksum(gWriteValue, sizeof(gWriteValue)));

  disconnectAndVerify();
}

}  // namespace

void setup()
{
  Serial.begin(115200);
  waitForSerial();
  Serial.println("RA4_BLE_LONG_GATT_CENTRAL:BOOT");
#if defined(LONG_GATT_PEER_GATE) || defined(LONG_GATT_DISCOVERY_PROBE)
  while (!Serial.available()) {
    Serial.println("RA4_BLE_LONG_GATT_CENTRAL:WAIT_START");
    delay(1000);
  }
  if (Serial.read() != 'S') {
    finish("START_COMMAND");
    return;
  }
#if defined(LONG_GATT_DISCOVERY_PROBE)
  const unsigned long probeStarted = millis();
  while (!Serial.available() && millis() - probeStarted < 2000) {}
  if (!Serial.available()) {
    finish("PROBE_GENERATION");
    return;
  }
  gGeneration = static_cast<uint8_t>(Serial.read());
#endif
#endif

  gBleBegan = BLE.begin();
  Serial.println(
      gBleBegan
          ? "RA4_BLE_LONG_GATT_CENTRAL:BLE_BEGIN=PASS"
          : "RA4_BLE_LONG_GATT_CENTRAL:BLE_BEGIN=FAIL");
  if (!gBleBegan) {
    finish("BLE_BEGIN");
    return;
  }
#if LONG_GATT_SESSIONS > 1 || defined(LONG_GATT_PEER_GATE) || defined(LONG_GATT_DISCOVERY_PROBE)
  BLE.setTimeout(15000);
#endif

  gScanning = BLE.scanForUuid(kServiceUuid) != 0;
  Serial.println(
      gScanning
          ? "RA4_BLE_LONG_GATT_CENTRAL:SCAN_START=PASS"
          : "RA4_BLE_LONG_GATT_CENTRAL:SCAN_START=FAIL");
  if (!gScanning) {
    finish("SCAN_START");
  }
  gScanStartedAt = millis();
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

  if (millis() - gScanStartedAt >= kScanTimeoutMs) {
    finish("SCAN_TIMEOUT");
  }
}
