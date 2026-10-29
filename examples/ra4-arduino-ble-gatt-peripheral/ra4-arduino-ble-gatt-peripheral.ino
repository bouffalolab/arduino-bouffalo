#include <ArduinoBLE.h>

namespace {

constexpr unsigned long kSerialReadyTimeoutMs = 2000;
constexpr uint8_t kInitialValue = 0x2a;

const char kServiceUuid[] = "9d1f1c20-7df8-4f79-9b7c-5a2b2bc9c001";
const char kCharacteristicUuid[] = "9d1f1c20-7df8-4f79-9b7c-5a2b2bc9c002";

BLEService gService(kServiceUuid);
BLEByteCharacteristic gCharacteristic(
    kCharacteristicUuid,
    BLERead | BLEWrite);

bool gConnected = false;
bool gWriteReported = false;
unsigned long gLastReadyAt = 0;

void waitForSerial()
{
  const unsigned long startedAt = millis();
  while (!Serial && millis() - startedAt < kSerialReadyTimeoutMs) {
  }
}

void printFailure(const char* reason)
{
  Serial.print("RA4_BLE_GATT_PERIPHERAL:RESULT=FAIL:");
  Serial.println(reason);
}

}  // namespace

void setup()
{
  Serial.begin(115200);
  waitForSerial();
  Serial.println("RA4_BLE_GATT_PERIPHERAL:BOOT");

  if (!BLE.begin()) {
    Serial.println("RA4_BLE_GATT_PERIPHERAL:BLE_BEGIN=FAIL");
    printFailure("BLE_BEGIN");
    return;
  }
  Serial.println("RA4_BLE_GATT_PERIPHERAL:BLE_BEGIN=PASS");

  BLE.setLocalName("R4GattGate");
  BLE.setAdvertisedService(gService);
  gService.addCharacteristic(gCharacteristic);
  BLE.addService(gService);
  gCharacteristic.writeValue(kInitialValue);

  if (!BLE.advertise()) {
    Serial.println("RA4_BLE_GATT_PERIPHERAL:ADVERTISE=FAIL");
    BLE.end();
    printFailure("ADVERTISE");
    return;
  }

  Serial.println("RA4_BLE_GATT_PERIPHERAL:READY");
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
            ? "RA4_BLE_GATT_PERIPHERAL:CENTRAL_CONNECTED"
            : "RA4_BLE_GATT_PERIPHERAL:CENTRAL_DISCONNECTED");
  }

  if (!gWriteReported && gCharacteristic.written()) {
    gWriteReported = true;
    Serial.println("RA4_BLE_GATT_PERIPHERAL:WRITE_RECEIVED");
  }

  if (!gConnected && millis() - gLastReadyAt >= 1000) {
    Serial.println("RA4_BLE_GATT_PERIPHERAL:READY");
    gLastReadyAt = millis();
  }
}
