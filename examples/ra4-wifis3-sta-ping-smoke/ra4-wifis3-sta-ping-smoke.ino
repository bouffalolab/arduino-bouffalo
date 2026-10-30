#include <WiFiS3.h>
#include <string.h>

namespace {

constexpr size_t kCommandCapacity = 208U;
constexpr unsigned long kWifiTimeoutMs = 30000UL;
constexpr char kUnknownHost[] = "bl616cl-ping-invalid.invalid";

char command[kCommandCapacity] = {};
size_t command_size = 0U;
bool completed = false;

void fail(const char* reason) {
  Serial.print("RA4_STA_PING:RESULT=FAIL:");
  Serial.println(reason);
  completed = true;
}

bool read_command_line() {
  while (Serial.available() > 0) {
    const int value = Serial.read();
    if (value < 0) {
      return false;
    }
    if (value == '\r') {
      continue;
    }
    if (value == '\n') {
      command[command_size] = '\0';
      command_size = 0U;
      return true;
    }
    if (command_size + 1U >= sizeof(command)) {
      command_size = 0U;
      fail("COMMAND");
      return false;
    }
    command[command_size++] = static_cast<char>(value);
  }
  return false;
}

int hex_value(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

bool decode_hex(char* encoded) {
  const size_t encoded_size = strlen(encoded);
  if ((encoded_size & 1U) != 0U) {
    return false;
  }
  const size_t decoded_size = encoded_size / 2U;
  for (size_t index = 0U; index < decoded_size; ++index) {
    const int high = hex_value(encoded[index * 2U]);
    const int low = hex_value(encoded[index * 2U + 1U]);
    if (high < 0 || low < 0) {
      return false;
    }
    encoded[index] = static_cast<char>((high << 4U) | low);
  }
  encoded[decoded_size] = '\0';
  return decoded_size != 0U;
}

bool parse_start(char*& ssid, char*& password) {
  constexpr char kPrefix[] = "START ";
  if (strncmp(command, kPrefix, sizeof(kPrefix) - 1U) != 0) {
    return false;
  }
  ssid = command + sizeof(kPrefix) - 1U;
  password = strchr(ssid, ' ');
  if (password == nullptr) {
    return false;
  }
  *password++ = '\0';
  return decode_hex(ssid) && decode_hex(password);
}

bool wait_for_station() {
  const unsigned long started_at = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - started_at >= kWifiTimeoutMs) {
      return false;
    }
    delay(20);
  }
  return true;
}

void run_smoke() {
  char* ssid = nullptr;
  char* password = nullptr;
  if (!parse_start(ssid, password)) {
    fail("START");
    return;
  }

  WiFi.setTimeout(kWifiTimeoutMs);
  if (WiFi.begin(ssid, password) != WL_CONNECTED && !wait_for_station()) {
    fail("WIFI_CONNECT");
    return;
  }
  Serial.println("RA4_STA_PING:WIFI_CONNECTED");

  const IPAddress gateway = WiFi.gatewayIP();
  if (gateway == IPAddress(0, 0, 0, 0)) {
    fail("GATEWAY");
    return;
  }
  Serial.println("RA4_STA_PING:GATEWAY_VALID");

  const int gateway_result = WiFi.ping(gateway, 64U, 2U);
  Serial.print("RA4_STA_PING:GATEWAY_RESULT=");
  Serial.println(gateway_result);
  if (gateway_result < 0) {
    fail("GATEWAY_PING");
    return;
  }
  Serial.println("RA4_STA_PING:GATEWAY_PASS");

  const int unknown_host = WiFi.ping(kUnknownHost, 64U, 1U);
  Serial.print("RA4_STA_PING:UNKNOWN_HOST_RESULT=");
  Serial.println(unknown_host);
  if (unknown_host != WL_PING_UNKNOWN_HOST) {
    fail("UNKNOWN_HOST");
    return;
  }
  Serial.println("RA4_STA_PING:UNKNOWN_HOST_PASS");

  WiFi.disconnect();
  completed = true;
  Serial.println("RA4_STA_PING:RESULT=PASS");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("RA4_STA_PING:BOOT");
  Serial.println("RA4_STA_PING:WAIT_START");
}

void loop() {
  if (!completed && read_command_line()) {
    run_smoke();
  }
}
