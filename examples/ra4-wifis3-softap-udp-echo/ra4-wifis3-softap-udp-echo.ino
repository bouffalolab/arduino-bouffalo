#include <WiFiS3.h>
#include <WiFiUdp.h>
#include <Modem.h>
#include <stdlib.h>
#include <string>
#include <string.h>

namespace {

constexpr uint16_t kListenPort = 4244;
constexpr uint16_t kSelfTestPort = 4245;
/*
 * The paired side is flashed and its RA4M1 runtime probe is uploaded after
 * this SoftAP becomes ready. Keep the peer alive for the whole automated
 * sequence, not merely for the final UDP exchange.
 */
constexpr unsigned long kPhaseTimeoutMs = 120000UL;
constexpr unsigned long kSelfTestTimeoutMs = 2000UL;
constexpr char kExpectedRequest[] = "bl616cl-udp-echo";
constexpr char kReply[] = "bl616cl-udp-reply";

char command[192] = {};
size_t command_size = 0U;
WiFiUDP udp;
WiFiUDP self_test;
bool completed = false;
unsigned long started_at = 0U;
unsigned int fragment_rounds = 0U;
uint32_t fragment_sequence = 1U;
bool waiting_fragment_ack = false;

void fail(const char* reason) {
  Serial.print("RA4_UDP_ECHO:RESULT=FAIL:");
  Serial.println(reason);
  udp.stop();
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

bool parse_start(char*& ssid, char*& password) {
  constexpr char kPrefix[] = "START ";
  if (strncmp(command, "FRAGSTART ", 10U) == 0) {
    char* end = nullptr;
    const unsigned long rounds = strtoul(command + 10U, &end, 10);
    if (rounds == 0U || rounds > 1000U || end == nullptr || *end != ' ') {
      return false;
    }
    fragment_rounds = static_cast<unsigned int>(rounds);
    ssid = end + 1U;
  } else if (strncmp(command, kPrefix, sizeof(kPrefix) - 1U) == 0) {
    ssid = command + sizeof(kPrefix) - 1U;
  } else {
    return false;
  }
  password = strchr(ssid, ' ');
  if (password == nullptr) {
    return false;
  }
  *password++ = '\0';
  return ssid[0] != '\0' && password[0] != '\0';
}

bool run_udp_loopback() {
  if (self_test.begin(kSelfTestPort) != 1 ||
      self_test.beginPacket(WiFi.softAPIP(), kListenPort) != 1 ||
      self_test.write(
          reinterpret_cast<const uint8_t*>(kExpectedRequest),
          sizeof(kExpectedRequest) - 1U) != sizeof(kExpectedRequest) - 1U ||
      self_test.endPacket() != 1) {
    return false;
  }

  const unsigned long deadline = millis() + kSelfTestTimeoutMs;
  while (millis() < deadline) {
    const int packet_size = udp.parsePacket();
    if (packet_size > 0) {
      if (packet_size != static_cast<int>(sizeof(kExpectedRequest) - 1U)) {
        return false;
      }
      char request[sizeof(kExpectedRequest)] = {};
      if (udp.read(request, sizeof(kExpectedRequest) - 1U) !=
              static_cast<int>(sizeof(kExpectedRequest) - 1U) ||
          memcmp(request, kExpectedRequest, sizeof(kExpectedRequest) - 1U) != 0 ||
          udp.beginPacket(udp.remoteIP(), udp.remotePort()) != 1 ||
          udp.write(
              reinterpret_cast<const uint8_t*>(kReply),
              sizeof(kReply) - 1U) != sizeof(kReply) - 1U ||
          udp.endPacket() != 1) {
        return false;
      }
      break;
    }
    delay(5);
  }

  while (millis() < deadline) {
    const int reply_size = self_test.parsePacket();
    if (reply_size > 0) {
      char reply[sizeof(kReply)] = {};
      const bool correct =
          reply_size == static_cast<int>(sizeof(kReply) - 1U) &&
          self_test.read(reply, sizeof(kReply) - 1U) ==
              static_cast<int>(sizeof(kReply) - 1U) &&
          memcmp(reply, kReply, sizeof(kReply) - 1U) == 0;
      self_test.stop();
      return correct;
    }
    delay(5);
  }
  self_test.stop();
  return false;
}

void start_echo() {
  char* ssid = nullptr;
  char* password = nullptr;
  if (!parse_start(ssid, password)) {
    fail("START");
    return;
  }
  // Keep the fixture independent of a previous AP+STA test subnet.
  WiFi.config(IPAddress(192, 168, 4, 1));
  if (WiFi.beginAP(ssid, password, 1) != WL_AP_LISTENING) {
    fail("AP");
    return;
  }
  const unsigned long ap_started_at = millis();
  while (WiFi.status() != WL_AP_LISTENING) {
    if (millis() - ap_started_at > kPhaseTimeoutMs) {
      fail("AP_STATUS");
      return;
    }
    delay(20);
  }
  if (udp.begin(kListenPort) != 1) {
    fail("UDP_BEGIN");
    return;
  }
  if (!run_udp_loopback()) {
    fail("UDP_LOOPBACK");
    return;
  }
  started_at = millis();
  Serial.print("RA4_UDP_ECHO:ADDRESS=");
  Serial.println(WiFi.softAPIP());
  Serial.println("RA4_UDP_ECHO:SELFTEST_PASS");
  Serial.println("RA4_UDP_ECHO:READY");
}

void print_fragment_diagnostic() {
  std::string diagnostic;
  Serial.print("RA4_UDP_ECHO:FRAGMENT_DIAG=");
  if (modem.write("+UDPDEBUG:", diagnostic, "AT+UDPDEBUG=0\r\n")) {
    Serial.println(diagnostic.c_str());
  } else {
    Serial.println("UNAVAILABLE");
  }
}

void service_fragment(const int packet_size) {
  uint8_t control[5] = {};
  if (packet_size != 5 || udp.read(control, sizeof(control)) != 5) {
    fail("FRAGMENT_CONTROL_SIZE");
    return;
  }
  const uint32_t sequence = static_cast<uint32_t>(control[1]) |
      (static_cast<uint32_t>(control[2]) << 8U) |
      (static_cast<uint32_t>(control[3]) << 16U) |
      (static_cast<uint32_t>(control[4]) << 24U);
  if (sequence != fragment_sequence ||
      control[0] != (waiting_fragment_ack ? 0x41U : 0x51U)) {
    fail("FRAGMENT_SEQUENCE");
    return;
  }
  if (waiting_fragment_ack) {
    Serial.print("RA4_UDP_ECHO:FRAGMENT_ROUND_PASS=");
    Serial.println(fragment_sequence);
    waiting_fragment_ack = false;
    started_at = millis();
    if (++fragment_sequence > fragment_rounds) {
      udp.stop();
      completed = true;
      Serial.println("RA4_UDP_ECHO:RESULT=PASS");
    }
    return;
  }
  if (udp.beginPacket(udp.remoteIP(), udp.remotePort()) != 1 ||
      udp.write(control + 1U, 4U) != 4U) {
    fail("FRAGMENT_BEGIN");
    return;
  }
  // Build the full bridge datagram in chunks without reserving 16 KiB on RA4M1.
  const uint8_t zeros[128] = {};
  for (size_t remaining = 16380U; remaining != 0U;) {
    const size_t count = remaining < sizeof(zeros) ? remaining : sizeof(zeros);
    if (udp.write(zeros, count) != count) {
      fail("FRAGMENT_WRITE");
      return;
    }
    remaining -= count;
  }
  if (udp.endPacket() != 1) {
    print_fragment_diagnostic();
    fail("FRAGMENT_SEND");
    return;
  }
  waiting_fragment_ack = true;
  started_at = millis();
}

void service_echo() {
  if (completed || started_at == 0U) {
    return;
  }
  if (waiting_fragment_ack && millis() - started_at > 8000UL) {
    print_fragment_diagnostic();
    fail("FRAGMENT_ACK_TIMEOUT");
    return;
  }
  if (millis() - started_at > kPhaseTimeoutMs) {
    if (fragment_rounds != 0U) print_fragment_diagnostic();
    fail("TIMEOUT");
    return;
  }

  const int packet_size = udp.parsePacket();
  if (packet_size <= 0) {
    return;
  }
  if (fragment_rounds != 0U) {
    service_fragment(packet_size);
    return;
  }
  if (packet_size != static_cast<int>(sizeof(kExpectedRequest) - 1U)) {
    fail("REQUEST_SIZE");
    return;
  }

  char received[sizeof(kExpectedRequest)] = {};
  const int received_size = udp.read(received, sizeof(kExpectedRequest) - 1U);
  if (received_size != static_cast<int>(sizeof(kExpectedRequest) - 1U) ||
      memcmp(received, kExpectedRequest, sizeof(kExpectedRequest) - 1U) != 0) {
    fail("REQUEST");
    return;
  }
  Serial.println("RA4_UDP_ECHO:RECEIVE_PASS");

  if (udp.beginPacket(udp.remoteIP(), udp.remotePort()) != 1 ||
      udp.write(
          reinterpret_cast<const uint8_t*>(kReply),
          sizeof(kReply) - 1U) != sizeof(kReply) - 1U ||
      udp.endPacket() != 1) {
    fail("REPLY");
    return;
  }
  Serial.println("RA4_UDP_ECHO:REPLY_PASS");
  udp.stop();
  completed = true;
  Serial.println("RA4_UDP_ECHO:RESULT=PASS");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("RA4_UDP_ECHO:BOOT");
  Serial.println("RA4_UDP_ECHO:WAIT_START");
}

void loop() {
  if (!completed && started_at == 0U && read_command_line()) {
    start_echo();
  }
  service_echo();
}
