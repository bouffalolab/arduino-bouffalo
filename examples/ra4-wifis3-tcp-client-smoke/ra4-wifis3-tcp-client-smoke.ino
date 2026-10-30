#include <WiFiS3.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr size_t kCommandCapacity = 160;
constexpr unsigned long kWifiTimeoutMs = 30000;
constexpr unsigned long kReceiveTimeoutMs = 10000;
constexpr char kExpectedEcho[] = "bl616cl-tcp-echo";

char command[kCommandCapacity] = {};
size_t command_size = 0U;
bool completed = false;

void fail(const char* reason) {
  Serial.print("RA4_TCP_CLIENT:RESULT=FAIL:");
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

bool parse_start(char*& ssid, char*& password, char*& host, uint16_t& port) {
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
  char* port_text = strchr(host, ' ');
  if (port_text == nullptr) {
    return false;
  }
  *port_text++ = '\0';
  const unsigned long parsed = strtoul(port_text, nullptr, 10);
  if (ssid[0] == '\0' || password[0] == '\0' || host[0] == '\0' ||
      parsed == 0U || parsed > 65535U) {
    return false;
  }
  port = static_cast<uint16_t>(parsed);
  return true;
}

void run_smoke() {
  char* ssid = nullptr;
  char* password = nullptr;
  char* host = nullptr;
  uint16_t port = 0U;
  if (!parse_start(ssid, password, host, port)) {
    fail("START");
    return;
  }

  if (WiFi.begin(ssid, password) != WL_CONNECTED) {
    fail("WIFI_CONNECT");
    return;
  }
  Serial.println("RA4_TCP_CLIENT:WIFI_CONNECTED");

  IPAddress resolved;
  if (WiFi.hostByName(host, resolved) != 1 ||
      resolved != IPAddress(192, 168, 4, 1)) {
    fail("HOST_BY_NAME");
    return;
  }
  Serial.println("RA4_TCP_CLIENT:HOST_BY_NAME_PASS");

  WiFiClient client;
  if (client.connect(host, port) != 1) {
    fail("TCP_CONNECT");
    return;
  }
  Serial.println("RA4_TCP_CLIENT:TCP_CONNECT_PASS");

  const size_t expected_size = sizeof(kExpectedEcho) - 1U;
  if (client.write(
          reinterpret_cast<const uint8_t*>(kExpectedEcho),
          expected_size) != expected_size) {
    client.stop();
    fail("TCP_SEND");
    return;
  }
  Serial.println("RA4_TCP_CLIENT:TCP_SEND_PASS");

  char received[sizeof(kExpectedEcho)] = {};
  size_t received_size = 0U;
  unsigned long availability_polls = 0U;
  unsigned long positive_availability_polls = 0U;
  unsigned long max_available = 0U;
  unsigned long successful_reads = 0U;
  const unsigned long started_at = millis();
  while (received_size < expected_size && millis() - started_at < kReceiveTimeoutMs) {
    const int available = client.available();
    ++availability_polls;
    if (available > 0) {
      ++positive_availability_polls;
      if (static_cast<unsigned long>(available) > max_available) {
        max_available = static_cast<unsigned long>(available);
      }
      const int value = client.read();
      if (value >= 0) {
        received[received_size++] = static_cast<char>(value);
        ++successful_reads;
      }
    } else {
      delay(10);
    }
  }
  Serial.print("RA4_TCP_CLIENT:TCP_AVAILABLE_POLLS=");
  Serial.println(availability_polls);
  Serial.print("RA4_TCP_CLIENT:TCP_AVAILABLE_POSITIVE=");
  Serial.println(positive_availability_polls);
  Serial.print("RA4_TCP_CLIENT:TCP_AVAILABLE_MAX=");
  Serial.println(max_available);
  Serial.print("RA4_TCP_CLIENT:TCP_READ_COUNT=");
  Serial.println(successful_reads);
  Serial.print("RA4_TCP_CLIENT:TCP_RECEIVE_BYTES=");
  Serial.println(received_size);
  client.stop();

  if (received_size != expected_size ||
      memcmp(received, kExpectedEcho, expected_size) != 0) {
    fail("TCP_ECHO");
    return;
  }

  Serial.println("RA4_TCP_CLIENT:TCP_RECEIVE_PASS");
  Serial.println("RA4_TCP_CLIENT:TCP_ECHO_PASS");
  Serial.println("RA4_TCP_CLIENT:RESULT=PASS");
  completed = true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("RA4_TCP_CLIENT:BOOT");
  Serial.println("RA4_TCP_CLIENT:WAIT_START");
}

void loop() {
  if (!completed && read_command_line()) {
    run_smoke();
  }
}
