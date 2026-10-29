#include <ArduinoBLE.h>
#if defined(ARDUINO_BLE_COEX_SNAPSHOT)
#include <utility/HCI.h>
#endif
#if defined(BLE_WIFI_COEX_GATE)
#include <Modem.h>
#include <WiFiS3.h>
#endif
#if defined(BLE_TLS_COEX_GATE)
#include "tls_coex_config.h"
#endif

namespace {

constexpr unsigned long kSerialReadyTimeoutMs = 2000;
constexpr size_t kValueBytes = 512;

const char kServiceUuid[] = "9d1f1c20-7df8-4f79-9b7c-5a2b2bc9c041";
const char kCharacteristicUuid[] = "9d1f1c20-7df8-4f79-9b7c-5a2b2bc9c042";

BLEService gService(kServiceUuid);
BLECharacteristic gCharacteristic(
    kCharacteristicUuid,
    BLERead | BLEWrite,
    kValueBytes,
    true);

bool gConnected = false;
bool gFailureReported = false;
bool gStopped = false;
uint8_t gWriteCount = 0;
unsigned long gLastReadyAt = 0;
uint8_t gValue[kValueBytes] = {};
#if defined(ARDUINO_BLE_COEX_SNAPSHOT)
int gLocalCs2 = -1;
#endif

void reportReady()
{
#if defined(ARDUINO_BLE_COEX_SNAPSHOT)
  Serial.print("BLE_COEX_HCI:LOCAL_CS2=");
  Serial.println(gLocalCs2);
#endif
  Serial.println("RA4_BLE_LONG_GATT_PERIPHERAL:READY");
}
#if defined(BLE_WIFI_COEX_GATE)
int gDiagnosticSlot = -1;

#if defined(BLE_TLS_COEX_GATE)
IPAddress gTlsEndpoint;
uint16_t gTlsPort = 0;
char gTlsSsid[33] = {};
char gTlsPassword[64] = {};

class CoexTlsClient : public WiFiSSLClient {
 public:
  int socket() const { return _sock; }
};

#if defined(BLE_TLS_HEAP_DIAGNOSTICS)
bool heapBoundary(unsigned cycle, const char* stage, int slot)
{
  std::string response;
  modem.begin();
  if (!modem.write("+SSLCLIENTDIAG:", response,
                   "AT+SSLCLIENTDIAG=%d\r\n", slot)) return false;
  // Read only the fixed numeric prefix, never arbitrary RAM or credentials.
  long fields[19] = {};
  const char* cursor = response.c_str();
  for (unsigned i = 0; i < 19; ++i) {
    char* end = nullptr;
    fields[i] = strtol(cursor, &end, 10);
    if (end == cursor || (i < 18 && *end != ',')) return false;
    cursor = end + (i < 18 ? 1 : 0);
  }
  Serial.print("BLE_TLS_COEX:HEAP_BOUNDARY=");
  Serial.print(cycle); Serial.print(','); Serial.print(stage);
  for (unsigned i = 0; i < 19; ++i) {
    Serial.print(','); Serial.print(fields[i]);
  }
  Serial.println();
  return true;
}
#endif

bool connectTlsStation()
{
  if (gTlsSsid[0] == '\0') return WiFi.status() == WL_CONNECTED;
  const int result = WiFi.begin(gTlsSsid, gTlsPassword);
  memset(gTlsSsid, 0, sizeof(gTlsSsid));
  memset(gTlsPassword, 0, sizeof(gTlsPassword));
  if (result != WL_CONNECTED) return false;
  Serial.println("BLE_TLS_COEX:STA_CONNECTED");
  return true;
}

bool startTlsStation()
{
  char command[256] = {};
  size_t size = 0;
  const unsigned long started = millis();
  while (millis() - started < 15000) {
    if (!Serial.available()) {
      delay(1);
      continue;
    }
    const int value = Serial.read();
    if (value == '\r') continue;
    if (value == '\n') break;
    if (value < 0 || size + 1 >= sizeof(command)) return false;
    command[size++] = static_cast<char>(value);
  }
  if (strncmp(command, "START ", 6) != 0) return false;
  char* fields[4] = {command + 6};
  for (unsigned i = 1; i < 4; ++i) {
    fields[i] = strchr(fields[i - 1], ' ');
    if (!fields[i]) return false;
    *fields[i]++ = '\0';
  }
  for (unsigned field = 0; field < 2; ++field) {
    const size_t length = strlen(fields[field]);
    if (length == 0 || length % 2 != 0) return false;
    for (size_t i = 0; i < length; i += 2) {
      unsigned byte = 0;
      for (unsigned digit = 0; digit < 2; ++digit) {
        const char c = fields[field][i + digit];
        if (c >= '0' && c <= '9') byte = byte * 16 + c - '0';
        else if (c >= 'a' && c <= 'f') byte = byte * 16 + c - 'a' + 10;
        else return false;
      }
      if (byte == 0) return false;
      fields[field][i / 2] = static_cast<char>(byte);
    }
    fields[field][length / 2] = '\0';
  }
  char* end = nullptr;
  const unsigned long port = strtoul(fields[3], &end, 10);
  if (*end != '\0' || port == 0 || port > 65535 ||
      !gTlsEndpoint.fromString(fields[2])) return false;
  gTlsPort = static_cast<uint16_t>(port);
  if (strlen(fields[0]) >= sizeof(gTlsSsid) ||
      strlen(fields[1]) >= sizeof(gTlsPassword)) return false;
  strcpy(gTlsSsid, fields[0]);
  strcpy(gTlsPassword, fields[1]);
  memset(command, 0, sizeof(command));
#if defined(BLE_TLS_STA_AFTER_CONNECT)
  Serial.println("BLE_TLS_COEX:STA_DEFERRED");
  return true;
#else
  return connectTlsStation();
#endif
}

// Read framing without Modem stripping the response's two CRLF separators.
bool tlsAtResponse(const char* request, char* response, size_t capacity)
{
  size_t size = 0;
  response[0] = '\0';
  while (Serial2.available()) (void)Serial2.read();
  Serial2.print(request);
  Serial2.flush();
  const unsigned long started = millis();
  while (millis() - started < 5000) {
    while (Serial2.available()) {
      const int value = Serial2.read();
      if (value < 0) continue;
      if (size + 1 >= capacity) return false;
      response[size++] = static_cast<char>(value);
      response[size] = '\0';
      if (size >= 4 && strcmp(response + size - 4, "OK\r\n") == 0) return true;
      if (size >= 7 && strcmp(response + size - 7, "ERROR\r\n") == 0) return false;
    }
    delay(1);
  }
  return false;
}

bool tlsAddressApi(int socket)
{
  const String local = WiFi.localIP().toString();
  const String remote = gTlsEndpoint.toString();
  char request[64] = {};
  char response[160] = {};
  char expected[128] = {};
  snprintf(request, sizeof(request), "AT+SSLIPCLIENT=%d\r\n", socket);
  snprintf(expected, sizeof(expected), "+SSLIPCLIENT: %s\r\n\r\nOK\r\n", local.c_str());
  if (!tlsAtResponse(request, response, sizeof(response)) ||
      strcmp(response, expected) != 0) return false;
  snprintf(request, sizeof(request), "AT+SSLCLIENTSTATE=%d\r\n", socket);
  snprintf(expected, sizeof(expected), "+SSLCLIENTSTATE: %s,%u,",
           remote.c_str(), static_cast<unsigned>(gTlsPort));
  if (!tlsAtResponse(request, response, sizeof(response)) ||
      strncmp(response, expected, strlen(expected)) != 0) return false;
  const char* cursor = response + strlen(expected);
  if (*cursor < '0' || *cursor > '9') return false;
  char* end = nullptr;
  const unsigned long localPort = strtoul(cursor, &end, 10);
  return localPort > 0 && localPort <= 65535 &&
      strcmp(end, "\r\n\r\nOK\r\n") == 0;
}

bool tlsExchange(unsigned cycle)
{
#if defined(BLE_TLS_STA_AFTER_CONNECT)
  if (!BLE.connected() || !connectTlsStation()) return false;
  BLE.poll();
#endif
  if (!BLE.connected() || WiFi.status() != WL_CONNECTED) return false;
#if defined(BLE_TLS_HEAP_DIAGNOSTICS)
  if (!heapBoundary(cycle, "BEFORE_TLS", gDiagnosticSlot)) return false;
#endif
  CoexTlsClient client;
  client.setCACert(kTlsCoexCa);
  client.setConnectionTimeout(10000);
  Serial.print("BLE_TLS_COEX:HANDSHAKE_START=");
  Serial.println(cycle);
  if (client.connect(gTlsEndpoint, gTlsPort) != 1) {
    Serial.println("BLE_TLS_COEX:FAIL=CONNECT");
    client.stop();
    return false;
  }
#if defined(BLE_TLS_HEAP_DIAGNOSTICS)
  if (!heapBoundary(cycle, "TLS_CONNECTED", client.socket())) {
    client.stop();
    return false;
  }
#endif
  if (!tlsAddressApi(client.socket())) {
    Serial.println("BLE_TLS_COEX:FAIL=ADDRESS_API");
    client.stop();
    return false;
  }
  Serial.print("BLE_TLS_COEX:ADDRESS_API_PASS=");
  Serial.println(cycle);
  constexpr char request[] = "bl616cl-tls-ping";
  constexpr char reply[] = "bl616cl-tls-echo";
  bool valid = client.write(reinterpret_cast<const uint8_t*>(request), sizeof(request) - 1)
      == sizeof(request) - 1;
  char received[sizeof(reply) - 1] = {};
  size_t size = 0;
  const unsigned long started = millis();
  while (valid && size < sizeof(received) && millis() - started < 10000) {
    BLE.poll();
    if (!BLE.connected()) { valid = false; break; }
    const int available = client.available();
    if (available > 0) {
      const int count = client.read(reinterpret_cast<uint8_t*>(received) + size,
                                    sizeof(received) - size);
      if (count > 0) size += static_cast<size_t>(count);
    } else delay(5);
  }
  valid = valid && size == sizeof(received) && memcmp(received, reply, sizeof(received)) == 0;
  client.stop();
  BLE.poll();
#if defined(BLE_TLS_HEAP_DIAGNOSTICS)
  if (!heapBoundary(cycle, "TLS_STOPPED", gDiagnosticSlot)) return false;
#endif
  if (!valid || !BLE.connected()) {
    Serial.println("BLE_TLS_COEX:FAIL=PAYLOAD_OR_CONNECTION");
    return false;
  }
  Serial.print("BLE_TLS_COEX:EXCHANGE_PASS=");
  Serial.println(cycle);
  return true;
}
#endif

bool heapSample(unsigned cycle)
{
#if defined(BLE_COEX_BASELINE)
  Serial.print("BLE_WIFI_COEX:HEAP_UNAVAILABLE=");
  Serial.println(cycle);
  return true;
#else
  std::string response;
  modem.begin();
  if (!modem.write("+SSLCLIENTDIAG:", response,
                   "AT+SSLCLIENTDIAG=%d\r\n", gDiagnosticSlot)) return false;
  unsigned long fields[15] = {};
  const char* cursor = response.c_str();
  for (unsigned i = 0; i < 15; ++i) {
    char* end = nullptr;
    fields[i] = strtoul(cursor, &end, 10);
    if (end == cursor || (i < 14 && *end != ',')) return false;
    cursor = end + (i < 14 ? 1 : 0);
  }
  Serial.print("BLE_WIFI_COEX:HEAP=");
  Serial.print(cycle);
  Serial.print(',');
  Serial.print(fields[13]);
  Serial.print(',');
  Serial.println(fields[14]);
  return true;
#endif
}

bool coexistenceCycle(unsigned cycle)
{
  const unsigned long startedAt = millis();
#if defined(BLE_TLS_COEX_GATE)
  if (!tlsExchange(cycle)) return false;
  const int networks = 0;
#elif defined(BLE_COEX_NO_WIFI_SCAN)
  Serial.print("BLE_WIFI_COEX:SCAN_SKIPPED=");
  Serial.println(cycle);
  // Control: preserve the approximate RA4 blocking interval with RF idle.
  delay(3100);
  const int networks = 0;
#else
  Serial.print("BLE_WIFI_COEX:SCAN_START=");
  Serial.println(cycle);
  const int networks = WiFi.scanNetworks();
  if (networks < 0) return false;
#endif
  Serial.println("BLE_WIFI_COEX:USB_ECHO_REQUEST");
  const unsigned long echoStarted = millis();
  while (!Serial.available() && millis() - echoStarted < 15000) {
    BLE.poll();
    delay(5);
  }
  if (!Serial.available() || Serial.read() != 'E') return false;
  BLE.poll();
  if (!BLE.connected() || !heapSample(cycle)) return false;
  Serial.print("BLE_WIFI_COEX:CYCLE=");
  Serial.print(cycle);
  Serial.print(',');
  Serial.print(networks);
  Serial.print(',');
  Serial.println(millis() - startedAt);
  return true;
}
#endif

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

void fillPayload(uint8_t generation)
{
  for (size_t i = 0; i < sizeof(gValue); ++i) {
    gValue[i] = payloadByte(generation, i);
  }
}

bool payloadMatches(const uint8_t* value, uint8_t generation)
{
  if (value == nullptr) {
    return false;
  }
  for (size_t i = 0; i < kValueBytes; ++i) {
    if (value[i] != payloadByte(generation, i)) {
      return false;
    }
  }
  return true;
}

void reportFailure(const char* reason)
{
  if (gFailureReported) {
    return;
  }
  gFailureReported = true;
  Serial.print("RA4_BLE_LONG_GATT_PERIPHERAL:RESULT=FAIL:");
  Serial.println(reason);
}

}  // namespace

void setup()
{
  Serial.begin(115200);
  waitForSerial();
  Serial.println("RA4_BLE_LONG_GATT_PERIPHERAL:BOOT");

#if defined(BLE_WIFI_COEX_GATE)
  while (!Serial.available()) {
    Serial.println("BLE_WIFI_COEX:WAIT_START");
    delay(1000);
  }
#if defined(BLE_TLS_COEX_GATE)
  WiFi.disconnect();
  if (!startTlsStation()) {
    reportFailure("TLS_START_COMMAND_OR_STA");
    return;
  }
#else
  if (Serial.read() != 'S') {
    reportFailure("START_COMMAND");
    return;
  }
  WiFi.disconnect();
#endif
#if !defined(BLE_COEX_BASELINE)
  std::string response;
  modem.begin();
  if (!modem.write("+SSLBEGINCLIENT:", response, "AT+SSLBEGINCLIENT\r\n")) {
    reportFailure("DIAGNOSTIC_SLOT");
    return;
  }
  gDiagnosticSlot = atoi(response.c_str());
  if (gDiagnosticSlot < 2000 || gDiagnosticSlot > 2007 || !heapSample(0)) {
    reportFailure("HEAP_BASELINE");
    return;
  }
#else
  heapSample(0);
#endif
#if defined(BLE_TLS_HEAP_DIAGNOSTICS)
  if (!heapBoundary(0, "BEFORE_BLE", gDiagnosticSlot)) {
    reportFailure("HEAP_DIAGNOSTIC"); return;
  }
#endif
#endif

  if (!BLE.begin()) {
    Serial.println("RA4_BLE_LONG_GATT_PERIPHERAL:BLE_BEGIN=FAIL");
    reportFailure("BLE_BEGIN");
    return;
  }
  Serial.println("RA4_BLE_LONG_GATT_PERIPHERAL:BLE_BEGIN=PASS");
#if defined(ARDUINO_BLE_COEX_SNAPSHOT)
  gLocalCs2 = HCI.readLeChannelSelection2();
#endif
#if defined(BLE_TLS_HEAP_DIAGNOSTICS)
  if (!heapBoundary(0, "AFTER_BLE", gDiagnosticSlot)) {
    reportFailure("HEAP_DIAGNOSTIC"); return;
  }
#endif

  fillPayload(0);
#if defined(LONG_GATT_PEER_GATE)
  // A nonzero value distinguishes a real GAP read from the API's fallback 0.
  BLE.setAppearance(0x1234);
#endif
  BLE.setLocalName("R4LongGattGate");
  BLE.setAdvertisedService(gService);
  gService.addCharacteristic(gCharacteristic);
  BLE.addService(gService);
  gCharacteristic.writeValue(gValue, sizeof(gValue));

  if (!BLE.advertise()) {
    Serial.println("RA4_BLE_LONG_GATT_PERIPHERAL:ADVERTISE=FAIL");
    reportFailure("ADVERTISE");
    return;
  }
#if defined(BLE_TLS_HEAP_DIAGNOSTICS)
  if (!heapBoundary(0, "ADVERTISING", gDiagnosticSlot)) {
    reportFailure("HEAP_DIAGNOSTIC"); return;
  }
#endif

  reportReady();
  gLastReadyAt = millis();
}

void loop()
{
  if (gFailureReported || gStopped) {
    return;
  }

  BLE.poll();

  const bool connected = BLE.connected();
#if defined(ARDUINO_BLE_COEX_SNAPSHOT) || defined(BLE_DISCOVERY_GATE)
  if (Serial.available()) {
    const int command = Serial.read();
#if defined(ARDUINO_BLE_COEX_SNAPSHOT)
    if (command == 'D') HCI.printCoexSnapshot(Serial);
#endif
#if defined(BLE_DISCOVERY_GATE)
    if (command == 'Q') {
      BLE.stopAdvertise();
      BLE.end();
      gStopped = true;
      Serial.println("RA4_BLE_DISCOVERY:STOPPED");
      return;
    }
#endif
  }
#endif
  if (connected != gConnected) {
    gConnected = connected;
    Serial.println(
        connected
            ? "RA4_BLE_LONG_GATT_PERIPHERAL:CENTRAL_CONNECTED"
            : "RA4_BLE_LONG_GATT_PERIPHERAL:CENTRAL_DISCONNECTED");
  }

  if (gCharacteristic.written()) {
    const uint8_t expectedGeneration = static_cast<uint8_t>(gWriteCount + 1);
    if (gCharacteristic.valueLength() != static_cast<int>(kValueBytes) ||
        !payloadMatches(gCharacteristic.value(), expectedGeneration)) {
      reportFailure("WRITE_VALUE");
      return;
    }

    ++gWriteCount;
    Serial.print("RA4_BLE_LONG_GATT_PERIPHERAL:WRITE_RECEIVED:COUNT=");
    Serial.print(gWriteCount);
    Serial.print(":LENGTH=");
    Serial.print(gCharacteristic.valueLength());
    Serial.print(":CHECKSUM=");
    Serial.println(payloadChecksum(gCharacteristic.value(), kValueBytes));
#if defined(BLE_WIFI_COEX_GATE)
    if (!coexistenceCycle(gWriteCount)) {
      reportFailure("CONNECTED_COEXISTENCE");
      return;
    }
#endif
  }

  if (!gConnected && millis() - gLastReadyAt >= 1000) {
    reportReady();
    gLastReadyAt = millis();
  }
}
