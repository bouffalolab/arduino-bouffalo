#include <WiFiS3.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr size_t kCommandCapacity = 192U;
constexpr unsigned long kWifiTimeoutMs = 30000UL;
constexpr unsigned long kTimeTimeoutMs = 20000UL;
constexpr unsigned long kReadyRepeatIntervalMs = 1000UL;
constexpr unsigned long kMinimumEpoch = 946684800UL;

char command[kCommandCapacity] = {};
size_t command_size = 0U;
bool completed = false;
unsigned long next_ready_announcement_ms = 0UL;

void result(unsigned long epoch) {
  Serial.print("RA4_GETTIME:EPOCH=");
  Serial.println(epoch);
  Serial.println("RA4_GETTIME:RESULT=PASS");
  completed = true;
}

void fail(const char* reason) {
  Serial.print("RA4_GETTIME:RESULT=FAIL:");
  Serial.println(reason);
  completed = true;
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

bool parse_start(char*& ssid, char*& password) {
  constexpr char kPrefix[] = "START ";
  if (strncmp(command, kPrefix, sizeof(kPrefix) - 1U) != 0) {
    return false;
  }
  ssid = command + sizeof(kPrefix) - 1U;
  password = strchr(ssid, ' ');
  if (password == nullptr || strchr(password + 1U, ' ') != nullptr) {
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
  Serial.println("RA4_GETTIME:WIFI_CONNECTED");

  const unsigned long started_at = millis();
  unsigned long epoch = 0UL;
  do {
    epoch = WiFi.getTime();
    if (epoch >= kMinimumEpoch) {
      WiFi.disconnect();
      result(epoch);
      return;
    }
    delay(250);
  } while (millis() - started_at < kTimeTimeoutMs);

  WiFi.disconnect();
  fail("TIMEOUT");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("RA4_GETTIME:BOOT");
  Serial.println("RA4_GETTIME:READY");
  next_ready_announcement_ms = millis() + kReadyRepeatIntervalMs;
}

void loop() {
  if (completed) {
    return;
  }
  if (read_command_line()) {
    run_smoke();
    return;
  }
  if (static_cast<long>(millis() - next_ready_announcement_ms) >= 0L) {
    Serial.println("RA4_GETTIME:READY");
    next_ready_announcement_ms = millis() + kReadyRepeatIntervalMs;
  }
}
