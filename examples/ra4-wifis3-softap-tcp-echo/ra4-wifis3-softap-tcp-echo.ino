#include <WiFiS3.h>
#include <string.h>

namespace {

constexpr uint16_t kListenPort = 4242;
constexpr size_t kCommandCapacity = 128;
constexpr unsigned long kStartTimeoutMs = 30000;
constexpr unsigned long kEchoIdleTimeoutMs = 30000;

WiFiServer server(kListenPort);
char command[kCommandCapacity] = {};
size_t command_size = 0U;
bool started = false;

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
      Serial.println("RA4_TCP_ECHO:RESULT=FAIL:COMMAND");
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

void start_ap() {
  char* ssid = nullptr;
  char* password = nullptr;
  if (!parse_start(ssid, password)) {
    Serial.println("RA4_TCP_ECHO:RESULT=FAIL:START");
    return;
  }

  if (WiFi.beginAP(ssid, password, 1) != WL_AP_LISTENING) {
    Serial.println("RA4_TCP_ECHO:RESULT=FAIL:AP");
    return;
  }

  const unsigned long started_at = millis();
  while (WiFi.status() != WL_AP_LISTENING) {
    if (millis() - started_at > kStartTimeoutMs) {
      Serial.println("RA4_TCP_ECHO:RESULT=FAIL:AP_STATUS");
      return;
    }
    delay(20);
  }

  server.begin();
  started = true;
  Serial.println("RA4_TCP_ECHO:READY");
}

void echo_once() {
  WiFiClient client = server.available();
  if (!client) {
    return;
  }

  const unsigned long started_at = millis();
  bool received_data = false;
  while (client.connected() && millis() - started_at < kEchoIdleTimeoutMs) {
    while (client.available() > 0) {
      const int value = client.read();
      if (value >= 0) {
        received_data = true;
        client.write(static_cast<uint8_t>(value));
      }
    }
    delay(1);
  }
  client.stop();
  Serial.println(received_data
      ? "RA4_TCP_ECHO:EXCHANGE"
      : "RA4_TCP_ECHO:EXCHANGE_EMPTY");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("RA4_TCP_ECHO:BOOT");
}

void loop() {
  if (!started && read_command_line()) {
    start_ap();
  }
  if (started) {
    echo_once();
  }
}
