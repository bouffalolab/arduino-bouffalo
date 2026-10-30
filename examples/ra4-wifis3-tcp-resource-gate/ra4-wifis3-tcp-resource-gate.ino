#include <WiFiS3.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr size_t kCommandCapacity = 224U;
constexpr size_t kConcurrentClientCount = 4U;
constexpr size_t kStressCycles = 3U;
constexpr size_t kMiddleClientIndex = 1U;
constexpr unsigned long kWifiTimeoutMs = 30000UL;
constexpr unsigned long kConnectTimeoutMs = 5000UL;
constexpr unsigned long kReadTimeoutMs = 10000UL;
constexpr unsigned long kReadyRepeatIntervalMs = 1000UL;
constexpr char kRequestPrefix[] = "bl616cl-tcp-resource-";
constexpr char kReplyPrefix[] = "bl616cl-tcp-resource-echo:";

char command[kCommandCapacity] = {};
size_t command_size = 0U;
bool completed = false;
unsigned long next_ready_announcement_ms = 0UL;
WiFiClient clients[kConcurrentClientCount];
WiFiClient reused_client;
WiFiClient refused_client;
WiFiClient recovery_client;

void fail(const char *reason) {
  Serial.print("RA4_TCP_RESOURCE:RESULT=FAIL:");
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

bool decode_hex(char *encoded) {
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
    char *&ssid,
    char *&password,
    IPAddress &peer,
    uint16_t &port,
    bool &stress) {
  constexpr char kPrefix[] = "START ";
  if (strncmp(command, kPrefix, sizeof(kPrefix) - 1U) != 0) {
    return false;
  }

  char *fields[4] = {};
  fields[0] = command + sizeof(kPrefix) - 1U;
  for (size_t index = 1U; index < sizeof(fields) / sizeof(fields[0]); ++index) {
    fields[index] = strchr(fields[index - 1U], ' ');
    if (fields[index] == nullptr) {
      return false;
    }
    *fields[index]++ = '\0';
  }
  char *mode = strchr(fields[3], ' ');
  if (mode != nullptr) {
    *mode++ = '\0';
  }
  if (mode != nullptr && strcmp(mode, "STRESS") != 0) {
    return false;
  }

  char *end = nullptr;
  const unsigned long parsed_port = strtoul(fields[3], &end, 10);
  ssid = fields[0];
  password = fields[1];
  stress = mode != nullptr;
  return decode_hex(ssid) &&
      decode_hex(password) &&
      peer.fromString(fields[2]) &&
      end != fields[3] &&
      *end == '\0' &&
      parsed_port > 0U &&
      parsed_port < 65535U &&
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

bool endpoint_matches(WiFiClient &client, const IPAddress &peer, uint16_t port) {
  return client.connected() == 1U &&
      client.remoteIP() == peer &&
      client.remotePort() == port;
}

bool exchange(WiFiClient &client, const char *suffix) {
  char request[48] = {};
  const int request_size = snprintf(
      request,
      sizeof(request),
      "%s%s",
      kRequestPrefix,
      suffix);
  if (request_size <= 0 ||
      static_cast<size_t>(request_size) >= sizeof(request) ||
      client.write(
          reinterpret_cast<const uint8_t *>(request),
          static_cast<size_t>(request_size)) != static_cast<size_t>(request_size)) {
    return false;
  }

  char expected[sizeof(kReplyPrefix) + sizeof(request)] = {};
  const int expected_size = snprintf(
      expected,
      sizeof(expected),
      "%s%s",
      kReplyPrefix,
      request);
  if (expected_size <= 0 ||
      static_cast<size_t>(expected_size) >= sizeof(expected)) {
    return false;
  }

  char received[sizeof(expected)] = {};
  size_t received_size = 0U;
  const unsigned long started_at = millis();
  while (received_size < static_cast<size_t>(expected_size) &&
      millis() - started_at < kReadTimeoutMs) {
    if (client.available() <= 0) {
      delay(10);
      continue;
    }
    const int value = client.read();
    if (value >= 0) {
      received[received_size++] = static_cast<char>(value);
    }
  }
  return received_size == static_cast<size_t>(expected_size) &&
      memcmp(received, expected, received_size) == 0;
}

void close_all_clients() {
  for (size_t index = 0U; index < kConcurrentClientCount; ++index) {
    clients[index].stop();
  }
  reused_client.stop();
  refused_client.stop();
  recovery_client.stop();
}

bool connect_to_peer(WiFiClient &client, const IPAddress &peer, uint16_t port) {
  client.setConnectionTimeout(kConnectTimeoutMs);
  return client.connect(peer, port) == 1 && endpoint_matches(client, peer, port);
}

bool connect_station(const char *ssid, const char *password) {
  if (WiFi.begin(ssid, password) != WL_CONNECTED && !wait_for_station()) {
    return false;
  }
  return true;
}

bool run_cycle(const IPAddress &peer, uint16_t port, size_t cycle) {
  Serial.println("RA4_TCP_RESOURCE:WIFI_CONNECTED");

  for (size_t index = 0U; index < kConcurrentClientCount; ++index) {
    if (!connect_to_peer(clients[index], peer, port)) {
      fail("INITIAL_CONNECT");
      return false;
    }
  }
  Serial.println("RA4_TCP_RESOURCE:INITIAL_CONNECT_PASS");
  Serial.println("RA4_TCP_RESOURCE:INITIAL_ENDPOINT_PASS");

  for (size_t index = 0U; index < kConcurrentClientCount; ++index) {
    char suffix[12] = {};
    snprintf(suffix, sizeof(suffix), "c%u-s%u",
             static_cast<unsigned int>(cycle),
             static_cast<unsigned int>(index));
    if (!exchange(clients[index], suffix)) {
      fail("INITIAL_ECHO");
      return false;
    }
  }
  Serial.println("RA4_TCP_RESOURCE:INITIAL_ECHO_PASS");

  clients[kMiddleClientIndex].stop();
  Serial.println("RA4_TCP_RESOURCE:MIDDLE_CLOSE_PASS");
  char reuse_suffix[12] = {};
  snprintf(reuse_suffix, sizeof(reuse_suffix), "c%u-reuse",
           static_cast<unsigned int>(cycle));
  if (!connect_to_peer(reused_client, peer, port) ||
      !exchange(reused_client, reuse_suffix)) {
    fail("MIDDLE_REUSE");
    return false;
  }
  Serial.println("RA4_TCP_RESOURCE:MIDDLE_REUSE_PASS");

  refused_client.setConnectionTimeout(kConnectTimeoutMs);
  if (refused_client.connect(peer, static_cast<uint16_t>(port + 1U)) != 0) {
    fail("REFUSED_CONNECT");
    return false;
  }
  refused_client.stop();
  Serial.println("RA4_TCP_RESOURCE:REFUSED_CONNECT_PASS");

  char recovery_suffix[12] = {};
  snprintf(recovery_suffix, sizeof(recovery_suffix), "c%u-recover",
           static_cast<unsigned int>(cycle));
  if (!connect_to_peer(recovery_client, peer, port) ||
      !exchange(recovery_client, recovery_suffix)) {
    fail("REFUSED_RELEASE");
    return false;
  }
  Serial.println("RA4_TCP_RESOURCE:REFUSED_RELEASE_PASS");

  close_all_clients();
  Serial.println("RA4_TCP_RESOURCE:CLOSE_ALL_PASS");
  return true;
}

void run_gate() {
  char *ssid = nullptr;
  char *password = nullptr;
  IPAddress peer;
  uint16_t port = 0U;
  bool stress = false;
  if (!parse_start(ssid, password, peer, port, stress)) {
    fail("START");
    return;
  }

  WiFi.setTimeout(kWifiTimeoutMs);
  const unsigned long started_at = millis();
  const size_t cycles = stress ? kStressCycles : 1U;
  for (size_t cycle = 0U; cycle < cycles; ++cycle) {
    if (!connect_station(ssid, password)) {
      close_all_clients();
      WiFi.disconnect();
      fail("WIFI_CONNECT");
      return;
    }
    if (!run_cycle(peer, port, cycle)) {
      close_all_clients();
      WiFi.disconnect();
      return;
    }
    WiFi.disconnect();
    if (stress) {
      const unsigned long disconnected_at = millis();
      while (WiFi.status() == WL_CONNECTED &&
          millis() - disconnected_at < kWifiTimeoutMs) {
        delay(20);
      }
      if (WiFi.status() == WL_CONNECTED) {
        fail("DISCONNECT");
        return;
      }
      Serial.print("RA4_TCP_RESOURCE:CYCLE_PASS=");
      Serial.println(cycle + 1U);
    }
  }
  if (stress) {
    Serial.print("RA4_TCP_RESOURCE:ELAPSED_MS=");
    Serial.println(millis() - started_at);
  }
  Serial.println("RA4_TCP_RESOURCE:RESULT=PASS");
  completed = true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("RA4_TCP_RESOURCE:READY");
  next_ready_announcement_ms = millis() + kReadyRepeatIntervalMs;
}

void loop() {
  if (completed) {
    delay(10);
    return;
  }
  if (static_cast<long>(millis() - next_ready_announcement_ms) >= 0L) {
    Serial.println("RA4_TCP_RESOURCE:READY");
    next_ready_announcement_ms += kReadyRepeatIntervalMs;
  }
  if (read_command_line()) {
    Serial.println("RA4_TCP_RESOURCE:START_RECEIVED");
    run_gate();
  }
}
