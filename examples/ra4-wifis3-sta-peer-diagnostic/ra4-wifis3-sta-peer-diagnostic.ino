#include <WiFiS3.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr size_t kCommandCapacity = 224U;
constexpr unsigned long kWifiTimeoutMs = 30000UL;
constexpr unsigned long kReceiveTimeoutMs = 5000UL;
constexpr unsigned long kReadyRepeatIntervalMs = 1000UL;
constexpr char kRequest[] = "bl616cl-peer-ping";
constexpr char kReply[] = "bl616cl-peer-echo";

char command[kCommandCapacity] = {};
size_t command_size = 0U;
bool completed = false;
unsigned long next_ready_announcement_ms = 0UL;

void result(const char* outcome) {
  Serial.print("RA4_STA_PEER:RESULT=");
  Serial.println(outcome);
  completed = true;
}

void fail(const char* reason) {
  Serial.print("RA4_STA_PEER:RESULT=FAIL:");
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

bool parse_start(
    char*& ssid,
    char*& password,
    IPAddress& peer,
    uint16_t& port) {
  constexpr char kPrefix[] = "START ";
  if (strncmp(command, kPrefix, sizeof(kPrefix) - 1U) != 0) {
    return false;
  }

  char* fields[4] = {};
  fields[0] = command + sizeof(kPrefix) - 1U;
  for (size_t index = 1U; index < sizeof(fields) / sizeof(fields[0]); ++index) {
    fields[index] = strchr(fields[index - 1U], ' ');
    if (fields[index] == nullptr) {
      return false;
    }
    *fields[index]++ = '\0';
  }
  if (strchr(fields[3], ' ') != nullptr) {
    return false;
  }

  const unsigned long parsed_port = strtoul(fields[3], nullptr, 10);
  ssid = fields[0];
  password = fields[1];
  return decode_hex(ssid) &&
      decode_hex(password) &&
      peer.fromString(fields[2]) &&
      parsed_port > 0U &&
      parsed_port <= 65535U &&
      (port = static_cast<uint16_t>(parsed_port), true);
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

bool exchange_with_peer(const IPAddress& peer, uint16_t port) {
  WiFiClient client;
  if (client.connect(peer, port) != 1) {
    return false;
  }
  Serial.println("RA4_STA_PEER:TCP_CONNECT_PASS");

  const size_t request_size = sizeof(kRequest) - 1U;
  if (client.write(reinterpret_cast<const uint8_t*>(kRequest), request_size) !=
      request_size) {
    client.stop();
    return false;
  }
  Serial.println("RA4_STA_PEER:TCP_WRITE_PASS");

  char received[sizeof(kReply)] = {};
  size_t received_size = 0U;
  const unsigned long started_at = millis();
  while (received_size < sizeof(kReply) - 1U &&
      millis() - started_at < kReceiveTimeoutMs) {
    if (client.available() <= 0) {
      delay(10);
      continue;
    }
    const int value = client.read();
    if (value >= 0) {
      received[received_size++] = static_cast<char>(value);
    }
  }
  client.stop();
  if (received_size != sizeof(kReply) - 1U ||
      memcmp(received, kReply, sizeof(kReply) - 1U) != 0) {
    return false;
  }
  Serial.println("RA4_STA_PEER:TCP_ECHO_PASS");
  return true;
}

void run_diagnostic() {
  char* ssid = nullptr;
  char* password = nullptr;
  IPAddress peer;
  uint16_t port = 0U;
  if (!parse_start(ssid, password, peer, port)) {
    fail("START");
    return;
  }

  WiFi.setTimeout(kWifiTimeoutMs);
  if (WiFi.begin(ssid, password) != WL_CONNECTED && !wait_for_station()) {
    fail("WIFI_CONNECT");
    return;
  }
  Serial.println("RA4_STA_PEER:WIFI_CONNECTED");

  const IPAddress gateway = WiFi.gatewayIP();
  if (gateway == IPAddress(0, 0, 0, 0)) {
    WiFi.disconnect();
    fail("GATEWAY");
    return;
  }
  Serial.println("RA4_STA_PEER:GATEWAY_VALID");
  if (WiFi.ping(gateway, 64U, 2U) < 0) {
    WiFi.disconnect();
    fail("GATEWAY_PING");
    return;
  }
  Serial.println("RA4_STA_PEER:GATEWAY_PING_PASS");

  const int peer_ping = WiFi.ping(peer, 64U, 2U);
  Serial.print("RA4_STA_PEER:PEER_PING=");
  Serial.println(peer_ping >= 0 ? "PASS" : "FAIL");

  if (!exchange_with_peer(peer, port)) {
    WiFi.disconnect();
    fail("TCP");
    return;
  }

  WiFi.disconnect();
  result("PASS");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("RA4_STA_PEER:BOOT");
  Serial.println("RA4_STA_PEER:READY");
  next_ready_announcement_ms = millis() + kReadyRepeatIntervalMs;
}

void loop() {
  if (completed) {
    return;
  }
  if (read_command_line()) {
    run_diagnostic();
    return;
  }
  if (static_cast<long>(millis() - next_ready_announcement_ms) < 0L) {
    return;
  }
  Serial.println("RA4_STA_PEER:READY");
  next_ready_announcement_ms = millis() + kReadyRepeatIntervalMs;
}
