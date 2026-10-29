#include <WiFiS3.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr size_t kCommandCapacity = 160U;
constexpr uint16_t kPort = 4243;
constexpr unsigned long kReceiveTimeoutMs = 10000UL;
constexpr unsigned long kConnectTimeoutMs = 10000UL;
constexpr char kApHost[] = "192.168.42.1";
constexpr char kBroadcastProbe[] = "broadcast-probe";
constexpr char kBroadcastReply[] = "broadcast-reply";
constexpr char kAcceptedGreeting[] = "accepted-greeting";
constexpr char kAcceptedAck[] = "accepted-ack";
constexpr char kAcceptedReply[] = "accepted-reply";

char command[kCommandCapacity] = {};
size_t command_size = 0U;
bool completed = false;

void fail(const char* reason) {
  Serial.print("RA4_TCP_SERVER_CLIENT:RESULT=FAIL:");
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

bool read_exact(WiFiClient& client, const char* expected) {
  const size_t expected_size = strlen(expected);
  char received[32] = {};
  if (expected_size >= sizeof(received)) {
    return false;
  }
  size_t received_size = 0U;
  const unsigned long started_at = millis();
  while (received_size < expected_size &&
         millis() - started_at < kReceiveTimeoutMs) {
    if (client.available() > 0) {
      const int value = client.read();
      if (value >= 0) {
        received[received_size++] = static_cast<char>(value);
      }
    } else {
      delay(5);
    }
  }
  return received_size == expected_size &&
      memcmp(received, expected, expected_size) == 0;
}

bool write_exact(WiFiClient& client, const char* data) {
  const size_t size = strlen(data);
  return client.write(reinterpret_cast<const uint8_t*>(data), size) == size;
}

bool connect_with_retry(WiFiClient& client, const char* host) {
  const unsigned long started_at = millis();
  unsigned long attempts = 0U;
  while (millis() - started_at < kConnectTimeoutMs) {
    ++attempts;
    if (client.connect(host, kPort) == 1) {
      Serial.print("RA4_TCP_SERVER_CLIENT:CONNECT_ATTEMPTS=");
      Serial.println(attempts);
      return true;
    }
    client.stop();
    delay(250);
  }
  return false;
}

void run_smoke() {
  char* ssid = nullptr;
  char* password = nullptr;
  char* host = nullptr;
  if (!parse_start(ssid, password, host)) {
    fail("START");
    return;
  }
  if (WiFi.begin(ssid, password) != WL_CONNECTED) {
    fail("WIFI_CONNECT");
    return;
  }
  Serial.println("RA4_TCP_SERVER_CLIENT:WIFI_CONNECTED");

  WiFiClient broadcast_client;
  if (strcmp(host, kApHost) != 0 || !connect_with_retry(broadcast_client, host)) {
    broadcast_client.stop();
    fail("BROADCAST_CONNECT");
    return;
  }
  Serial.println("RA4_TCP_SERVER_CLIENT:BROADCAST_CONNECT_PASS");
  if (!write_exact(broadcast_client, kBroadcastProbe)) {
    broadcast_client.stop();
    fail("BROADCAST_SEND");
    return;
  }
  Serial.println("RA4_TCP_SERVER_CLIENT:BROADCAST_SEND_PASS");
  if (!read_exact(broadcast_client, kBroadcastReply)) {
    broadcast_client.stop();
    fail("BROADCAST_RECEIVE");
    return;
  }
  broadcast_client.stop();
  Serial.println("RA4_TCP_SERVER_CLIENT:BROADCAST_PASS");

  WiFiClient accepted_client;
  if (!connect_with_retry(accepted_client, host)) {
    accepted_client.stop();
    fail("ACCEPT_CONNECT");
    return;
  }
  Serial.println("RA4_TCP_SERVER_CLIENT:ACCEPT_CONNECT_PASS");
  if (!read_exact(accepted_client, kAcceptedGreeting)) {
    accepted_client.stop();
    fail("ACCEPT_GREETING");
    return;
  }
  Serial.println("RA4_TCP_SERVER_CLIENT:ACCEPT_GREETING_PASS");
  if (!write_exact(accepted_client, kAcceptedAck)) {
    accepted_client.stop();
    fail("ACCEPT_SEND");
    return;
  }
  if (!read_exact(accepted_client, kAcceptedReply)) {
    accepted_client.stop();
    fail("ACCEPT_RECEIVE");
    return;
  }
  accepted_client.stop();
  Serial.println("RA4_TCP_SERVER_CLIENT:ACCEPT_PASS");
  Serial.println("RA4_TCP_SERVER_CLIENT:RESULT=PASS");
  completed = true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("RA4_TCP_SERVER_CLIENT:BOOT");
  Serial.println("RA4_TCP_SERVER_CLIENT:WAIT_START");
}

void loop() {
  if (!completed && read_command_line()) {
    run_smoke();
  }
}
