#include <WiFiS3.h>
#include <string.h>

namespace {

constexpr uint16_t kListenPort = 4243;
constexpr size_t kCommandCapacity = 128U;
constexpr unsigned long kPhaseTimeoutMs = 30000UL;
const IPAddress kApAddress(192, 168, 42, 1);
constexpr char kBroadcastProbe[] = "broadcast-probe";
constexpr char kBroadcastReply[] = "broadcast-reply";
constexpr char kAcceptedGreeting[] = "accepted-greeting";
constexpr char kAcceptedAck[] = "accepted-ack";
constexpr char kAcceptedReply[] = "accepted-reply";

enum class Phase : uint8_t {
  WaitStart,
  WaitBroadcastClient,
  WaitAcceptedClient,
  WaitAcceptedAck,
  Done,
};

WiFiServer server(kListenPort);
WiFiClient broadcast_client;
WiFiClient accepted_client;
Phase phase = Phase::WaitStart;
char command[kCommandCapacity] = {};
size_t command_size = 0U;
unsigned long phase_started_at = 0U;
char broadcast_received[sizeof(kBroadcastProbe)] = {};
size_t broadcast_received_size = 0U;
char accepted_received[sizeof(kAcceptedAck)] = {};
size_t accepted_received_size = 0U;

void fail(const char* reason) {
  Serial.print("RA4_TCP_SERVER:RESULT=FAIL:");
  Serial.println(reason);
  broadcast_client.stop();
  accepted_client.stop();
  server.end();
  phase = Phase::Done;
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
  if (password == nullptr) {
    return false;
  }
  *password++ = '\0';
  return ssid[0] != '\0' && password[0] != '\0';
}

bool read_expected(
    WiFiClient& client,
    const char* expected,
    char* received,
    size_t& received_size,
    size_t capacity) {
  const size_t expected_size = strlen(expected);
  if (received == nullptr || expected_size >= capacity) {
    return false;
  }
  while (client.available() > 0 && received_size < expected_size) {
    const int value = client.read();
    if (value >= 0) {
      received[received_size++] = static_cast<char>(value);
    }
  }
  return received_size == expected_size &&
      memcmp(received, expected, expected_size) == 0;
}

void start_server() {
  char* ssid = nullptr;
  char* password = nullptr;
  if (!parse_start(ssid, password)) {
    fail("START");
    return;
  }
  WiFi.config(kApAddress);
  if (WiFi.beginAP(ssid, password, 1) != WL_AP_LISTENING) {
    fail("AP");
    return;
  }
  const unsigned long started_at = millis();
  while (WiFi.status() != WL_AP_LISTENING) {
    if (millis() - started_at > kPhaseTimeoutMs) {
      fail("AP_STATUS");
      return;
    }
    delay(20);
  }
  server.begin();
  if (!server) {
    fail("SERVER_BEGIN");
    return;
  }
  phase = Phase::WaitBroadcastClient;
  phase_started_at = millis();
  Serial.println("RA4_TCP_SERVER:READY");
}

void service_server() {
  if (phase == Phase::WaitBroadcastClient) {
    WiFiClient client = server.available();
    if (client) {
      broadcast_client = client;
      Serial.println("RA4_TCP_SERVER:BROADCAST_CLIENT");
    }
    if (broadcast_client && read_expected(
            broadcast_client,
            kBroadcastProbe,
            broadcast_received,
            broadcast_received_size,
            sizeof(broadcast_received))) {
      if (server.write(
              reinterpret_cast<const uint8_t*>(kBroadcastReply),
              sizeof(kBroadcastReply) - 1U) != sizeof(kBroadcastReply) - 1U) {
        fail("SERVERWRITE");
        return;
      }
      phase = Phase::WaitAcceptedClient;
      phase_started_at = millis();
      Serial.println("RA4_TCP_SERVER:BROADCAST_PASS");
    }
  } else if (phase == Phase::WaitAcceptedClient) {
    WiFiClient client = server.accept();
    if (client) {
      accepted_client = client;
      Serial.println("RA4_TCP_SERVER:ACCEPT_CLIENT");
      if (accepted_client.write(
              reinterpret_cast<const uint8_t*>(kAcceptedGreeting),
              sizeof(kAcceptedGreeting) - 1U) != sizeof(kAcceptedGreeting) - 1U) {
        fail("ACCEPT_SEND");
        return;
      }
      phase = Phase::WaitAcceptedAck;
      phase_started_at = millis();
      Serial.println("RA4_TCP_SERVER:ACCEPT_PASS");
    }
  } else if (phase == Phase::WaitAcceptedAck) {
    if (read_expected(
            accepted_client,
            kAcceptedAck,
            accepted_received,
            accepted_received_size,
            sizeof(accepted_received))) {
      if (accepted_client.write(
              reinterpret_cast<const uint8_t*>(kAcceptedReply),
              sizeof(kAcceptedReply) - 1U) != sizeof(kAcceptedReply) - 1U) {
        fail("ACCEPT_REPLY");
        return;
      }
      broadcast_client.stop();
      accepted_client.stop();
      server.end();
      phase = Phase::Done;
      Serial.println("RA4_TCP_SERVER:RESULT=PASS");
    }
  }

  if (phase != Phase::WaitStart && phase != Phase::Done &&
      millis() - phase_started_at > kPhaseTimeoutMs) {
    fail("TIMEOUT");
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("RA4_TCP_SERVER:BOOT");
}

void loop() {
  if (phase == Phase::WaitStart && read_command_line()) {
    start_server();
  }
  service_server();
  delay(1);
}
