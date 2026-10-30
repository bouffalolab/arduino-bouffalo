#include <WiFiS3.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr size_t kCommandCapacity = 192U;
constexpr unsigned long kWifiTimeoutMs = 30000UL;
constexpr char kUnknownHost[] = "bl616cl-ping-invalid.invalid";
constexpr char kTimeoutHost[] = "192.168.4.254";

char command[kCommandCapacity] = {};
size_t command_size = 0U;
bool completed = false;

void fail(const char* reason) {
  Serial.print("RA4_PING:RESULT=FAIL:");
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

bool parse_start(char*& ssid, char*& password, char*& host) {
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
  host = strchr(password, ' ');
  if (host == nullptr) {
    return false;
  }
  *host++ = '\0';
  return ssid[0] != '\0' && password[0] != '\0' && host[0] != '\0';
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

void print_result(const char* label, int result) {
  Serial.print("RA4_PING:");
  Serial.print(label);
  Serial.print("=");
  Serial.println(result);
}

void run_smoke() {
  char* ssid = nullptr;
  char* password = nullptr;
  char* host = nullptr;
  if (!parse_start(ssid, password, host)) {
    fail("START");
    return;
  }

  if (WiFi.begin(ssid, password) != WL_CONNECTED && !wait_for_station()) {
    fail("WIFI_CONNECT");
    return;
  }
  Serial.println("RA4_PING:WIFI_CONNECTED");

  const int success = WiFi.ping(host, 64U, 2U);
  print_result("SUCCESS_RESULT", success);
  if (success < 0) {
    fail("SUCCESS");
    return;
  }
  Serial.println("RA4_PING:SUCCESS_PASS");

  const int unknown_host = WiFi.ping(kUnknownHost, 64U, 1U);
  print_result("UNKNOWN_HOST_RESULT", unknown_host);
  if (unknown_host != WL_PING_UNKNOWN_HOST) {
    fail("UNKNOWN_HOST");
    return;
  }
  Serial.println("RA4_PING:UNKNOWN_HOST_PASS");

  const int timeout = WiFi.ping(kTimeoutHost, 64U, 1U);
  print_result("TIMEOUT_RESULT", timeout);
  if (timeout != WL_PING_TIMEOUT) {
    fail("TIMEOUT");
    return;
  }
  Serial.println("RA4_PING:TIMEOUT_PASS");

  WiFi.disconnect();
  completed = true;
  Serial.println("RA4_PING:RESULT=PASS");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("RA4_PING:BOOT");
  Serial.println("RA4_PING:WAIT_START");
}

void loop() {
  if (!completed && read_command_line()) {
    run_smoke();
  }
}
