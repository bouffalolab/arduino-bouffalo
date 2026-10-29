#include <WiFiS3.h>
#include <WiFiUdp.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr uint16_t kLocalPort = 4245;
constexpr unsigned long kWifiTimeoutMs = 30000UL;
constexpr unsigned long kReceiveTimeoutMs = 30000UL;
constexpr unsigned long kAttemptReceiveWindowMs = 2000UL;
constexpr uint8_t kSendAttempts = 8U;
constexpr char kRequest[] = "bl616cl-udp-echo";
constexpr char kExpectedReply[] = "bl616cl-udp-reply";

char command[224] = {};
size_t command_size = 0U;
bool completed = false;

void fail(const char* reason) {
  Serial.print("RA4_UDP_CLIENT:RESULT=FAIL:");
  Serial.println(reason);
  completed = true;
}

void print_udp_debug() {
  while (Serial2.available() > 0) {
    (void)Serial2.read();
  }
  Serial2.print("AT+UDPDEBUG=0\r\n");

  char line[160] = {};
  size_t line_size = 0U;
  const unsigned long deadline = millis() + 1500UL;
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
        constexpr char kPrefix[] = "+UDPDEBUG: ";
        if (strncmp(line, kPrefix, sizeof(kPrefix) - 1U) == 0) {
          Serial.print("RA4_UDP_CLIENT:UDP_DEBUG=");
          Serial.println(line + sizeof(kPrefix) - 1U);
          return;
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
  Serial.println("RA4_UDP_CLIENT:UDP_DEBUG=UNAVAILABLE");
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
  Serial.println("RA4_UDP_CLIENT:WIFI_CONNECTED");

  IPAddress expected_peer;
  IPAddress resolved;
  if (!expected_peer.fromString(host) ||
      WiFi.hostByName(host, resolved) != 1 ||
      resolved != expected_peer) {
    fail("HOST_BY_NAME");
    return;
  }
  Serial.println("RA4_UDP_CLIENT:HOST_BY_NAME_PASS");

  WiFiUDP udp;
  if (udp.begin(kLocalPort) != 1) {
    fail("UDP_BEGIN");
    return;
  }
  Serial.println("RA4_UDP_CLIENT:UDP_BEGIN_PASS");

  const unsigned long started_at = millis();
  int packet_size = 0;
  for (uint8_t attempt = 1U;
       attempt <= kSendAttempts && packet_size <= 0;
       ++attempt) {
    if (udp.beginPacket(resolved, port) != 1 ||
        udp.write(
            reinterpret_cast<const uint8_t*>(kRequest),
            sizeof(kRequest) - 1U) != sizeof(kRequest) - 1U ||
        udp.endPacket() != 1) {
      udp.stop();
      fail("UDP_SEND");
      return;
    }
    Serial.print("RA4_UDP_CLIENT:UDP_SEND_ATTEMPT=");
    Serial.println(attempt);
    if (attempt == 1U) {
      print_udp_debug();
    }

    const unsigned long attempt_started_at = millis();
    while (millis() - started_at < kReceiveTimeoutMs &&
           millis() - attempt_started_at < kAttemptReceiveWindowMs) {
      packet_size = udp.parsePacket();
      if (packet_size > 0) {
        break;
      }
      delay(10);
    }
  }
  Serial.println("RA4_UDP_CLIENT:UDP_SEND_PASS");
  if (packet_size != static_cast<int>(sizeof(kExpectedReply) - 1U)) {
    print_udp_debug();
    udp.stop();
    fail("UDP_PARSE");
    return;
  }

  if (udp.remoteIP() != expected_peer ||
      udp.remotePort() != port) {
    udp.stop();
    fail("UDP_REMOTE");
    return;
  }
  Serial.println("RA4_UDP_CLIENT:UDP_REMOTE_PASS");

  if (udp.available() != packet_size || udp.peek() !=
          static_cast<int>(static_cast<uint8_t>(kExpectedReply[0]))) {
    udp.stop();
    fail("UDP_AVAILABLE_PEEK");
    return;
  }

  char received[sizeof(kExpectedReply)] = {};
  const int first_read = udp.read(received, 5U);
  const int remaining = udp.available();
  const int second_read = udp.read(
      received + first_read,
      sizeof(kExpectedReply) - 1U - static_cast<size_t>(first_read));
  if (first_read != 5 || remaining !=
          static_cast<int>(sizeof(kExpectedReply) - 1U - 5U) ||
      second_read != static_cast<int>(sizeof(kExpectedReply) - 1U - 5U) ||
      memcmp(received, kExpectedReply, sizeof(kExpectedReply) - 1U) != 0) {
    udp.stop();
    fail("UDP_READ");
    return;
  }
  Serial.println("RA4_UDP_CLIENT:UDP_READ_PASS");

  udp.flush();
  udp.stop();
  Serial.println("RA4_UDP_CLIENT:RESULT=PASS");
  completed = true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("RA4_UDP_CLIENT:BOOT");
  Serial.println("RA4_UDP_CLIENT:WAIT_START");
}

void loop() {
  if (!completed && read_command_line()) {
    run_smoke();
  }
}
