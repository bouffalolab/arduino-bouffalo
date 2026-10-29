#include <WiFiS3.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr unsigned long kAtTimeoutMs = 20000UL;

char command[224] = {};
size_t command_size = 0U;
bool completed = false;

void fail(const char* reason) {
  Serial.print("RA4_UDP_RUNTIME:RESULT=FAIL:");
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
  char* end = nullptr;
  const unsigned long parsed = strtoul(port_text, &end, 10);
  if (ssid[0] == '\0' || password[0] == '\0' || host[0] == '\0' ||
      end == port_text || *end != '\0' || parsed == 0U || parsed > 65535U) {
    return false;
  }
  port = static_cast<uint16_t>(parsed);
  return true;
}

bool send_runtime_probe(const char* host, uint16_t port) {
  while (Serial2.available() > 0) {
    (void)Serial2.read();
  }

  Serial2.print("AT+UDPRUNTIME=4245,");
  Serial2.print(port);
  Serial2.print(",");
  Serial2.print(host);
  Serial2.print("\r\n");

  char line[160] = {};
  size_t line_size = 0U;
  bool saw_result = false;
  bool passed = false;
  const unsigned long deadline = millis() + kAtTimeoutMs;
  while (millis() < deadline) {
    while (Serial2.available() > 0) {
      const int value = Serial2.read();
      if (value < 0) {
        break;
      }
      if (value == '\r') {
        continue;
      }
      if (value == '\n') {
        line[line_size] = '\0';
        constexpr char kPrefix[] = "+UDPRUNTIME: ";
        if (strncmp(line, kPrefix, sizeof(kPrefix) - 1U) == 0) {
          saw_result = true;
          passed = strncmp(
              line + sizeof(kPrefix) - 1U,
              "1,0,",
              sizeof("1,0,") - 1U) == 0;
          Serial.print("RA4_UDP_RUNTIME:DIAGNOSTIC=");
          Serial.println(line + sizeof(kPrefix) - 1U);
        } else if (strcmp(line, "ERROR") == 0) {
          return false;
        } else if (strcmp(line, "OK") == 0 && saw_result) {
          return passed;
        }
        line_size = 0U;
        continue;
      }
      if (line_size + 1U < sizeof(line)) {
        line[line_size++] = static_cast<char>(value);
      } else {
        line_size = 0U;
      }
    }
    delay(5);
  }
  return false;
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
  Serial.println("RA4_UDP_RUNTIME:WIFI_CONNECTED");

  if (!send_runtime_probe(host, port)) {
    fail("DIRECT_UDP");
    return;
  }
  Serial.println("RA4_UDP_RUNTIME:RESULT=PASS");
  completed = true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200);
  Serial.println("RA4_UDP_RUNTIME:BOOT");
  Serial.println("RA4_UDP_RUNTIME:WAIT_START");
}

void loop() {
  if (!completed && read_command_line()) {
    run_smoke();
  }
}
