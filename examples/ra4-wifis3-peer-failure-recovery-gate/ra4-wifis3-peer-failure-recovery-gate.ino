#include <WiFiS3.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr unsigned long kWifiTimeoutMs = 30000;
constexpr unsigned long kIoTimeoutMs = 5000;
constexpr unsigned long kUdpDropTimeoutMs = 2500;
constexpr size_t kPayloadSize = 32;
char command[240] = {};
size_t command_size = 0;
bool finished = false;
WiFiClient tcp;
WiFiUDP udp;
uint8_t payload[kPayloadSize] = {};
uint8_t reply[kPayloadSize] = {};

void fail(const char *reason) {
  tcp.stop();
  udp.stop();
  WiFi.disconnect();
  Serial.print("RA4_PEER_FAILURE:RESULT=FAIL:");
  Serial.println(reason);
  finished = true;
}

int hex_digit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

bool decode_hex(char *field) {
  const size_t encoded = strlen(field);
  if (encoded == 0 || (encoded & 1U) != 0U) return false;
  for (size_t index = 0; index < encoded / 2U; ++index) {
    const int high = hex_digit(field[index * 2U]);
    const int low = hex_digit(field[index * 2U + 1U]);
    if (high < 0 || low < 0) return false;
    field[index] = static_cast<char>((high << 4U) | low);
  }
  field[encoded / 2U] = '\0';
  return true;
}

bool parse_start(char *&ssid, char *&password, IPAddress &peer,
                 uint16_t &tcp_port, uint16_t &udp_port) {
  if (strncmp(command, "START ", 6) != 0) return false;
  char *fields[5] = {command + 6};
  for (size_t index = 1; index < 5; ++index) {
    fields[index] = strchr(fields[index - 1], ' ');
    if (!fields[index]) return false;
    *fields[index]++ = '\0';
  }
  if (strchr(fields[4], ' ')) return false;
  char *tcp_end = nullptr;
  char *udp_end = nullptr;
  const unsigned long parsed_tcp = strtoul(fields[3], &tcp_end, 10);
  const unsigned long parsed_udp = strtoul(fields[4], &udp_end, 10);
  ssid = fields[0];
  password = fields[1];
  if (!decode_hex(ssid) || !decode_hex(password) ||
      !peer.fromString(fields[2]) || tcp_end == fields[3] ||
      udp_end == fields[4] || *tcp_end || *udp_end ||
      parsed_tcp == 0 || parsed_tcp > 65535 ||
      parsed_udp == 0 || parsed_udp > 65535) return false;
  tcp_port = static_cast<uint16_t>(parsed_tcp);
  udp_port = static_cast<uint16_t>(parsed_udp);
  return true;
}

bool wait_station() {
  const unsigned long started_at = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - started_at >= kWifiTimeoutMs) return false;
    delay(20);
  }
  return true;
}

bool read_exact(WiFiClient &client, size_t size) {
  size_t received = 0;
  const unsigned long started_at = millis();
  while (received < size && millis() - started_at < kIoTimeoutMs) {
    const int count = client.read(reply + received, size - received);
    if (count > 0) {
      received += static_cast<size_t>(count);
    } else {
      delay(10);
    }
  }
  return received == size;
}

bool run_tcp_failure_recovery(IPAddress peer, uint16_t port) {
  tcp.setConnectionTimeout(10000);
  if (tcp.connect(peer, port) != 1) return false;
  delay(1200);
  const int connected_after_reset = tcp.connected() ? 1 : 0;
  Serial.print("RA4_PEER_FAILURE:TCP_CONNECTED_AFTER_RST=");
  Serial.println(connected_after_reset);
  for (size_t index = 0; index < sizeof(payload); ++index) {
    payload[index] = static_cast<uint8_t>(0x30U + index);
  }
  // The first peer session is reset. The second write must expose that error.
  const size_t sent = tcp.write(payload, sizeof(payload));
  Serial.print("RA4_PEER_FAILURE:TCP_WRITE1=");
  Serial.println(static_cast<unsigned long>(sent));
  if (sent == sizeof(payload)) {
    delay(300);
    const size_t retry_sent = tcp.write(payload, sizeof(payload));
    Serial.print("RA4_PEER_FAILURE:TCP_WRITE2=");
    Serial.println(static_cast<unsigned long>(retry_sent));
    if (retry_sent == sizeof(payload)) return false;
  }
  tcp.stop();
  Serial.println("RA4_PEER_FAILURE:TCP_RST_ERROR_PASS");

  if (tcp.connect(peer, port) != 1 ||
      tcp.write(payload, sizeof(payload)) != sizeof(payload) ||
      !read_exact(tcp, sizeof(reply)) ||
      memcmp(reply, payload, sizeof(reply)) != 0) {
    return false;
  }
  tcp.stop();
  Serial.println("RA4_PEER_FAILURE:TCP_RECOVERY_PASS");
  return true;
}

bool run_udp_failure_recovery(IPAddress peer, uint16_t port) {
  for (size_t index = 0; index < sizeof(payload); ++index) {
    payload[index] = static_cast<uint8_t>(0xA0U + index);
  }
  if (udp.begin(25210) != 1 ||
      udp.beginPacket(peer, port) != 1 ||
      udp.write(payload, sizeof(payload)) != sizeof(payload) ||
      udp.endPacket() != 1) return false;

  const unsigned long started_at = millis();
  while (millis() - started_at < kUdpDropTimeoutMs) {
    if (udp.parsePacket() > 0) return false;
    delay(10);
  }
  udp.stop();
  Serial.println("RA4_PEER_FAILURE:UDP_TIMEOUT_PASS");

  if (udp.begin(25210) != 1 ||
      udp.beginPacket(peer, port) != 1 ||
      udp.write(payload, sizeof(payload)) != sizeof(payload) ||
      udp.endPacket() != 1) return false;
  const unsigned long retry_started_at = millis();
  int packet_size = 0;
  while (millis() - retry_started_at < kIoTimeoutMs) {
    packet_size = udp.parsePacket();
    if (packet_size > 0) break;
    delay(10);
  }
  if (packet_size != static_cast<int>(sizeof(reply)) ||
      udp.remoteIP() != peer || udp.remotePort() != port ||
      udp.read(reply, sizeof(reply)) != static_cast<int>(sizeof(reply)) ||
      memcmp(reply, payload, sizeof(reply)) != 0) return false;
  udp.stop();
  Serial.println("RA4_PEER_FAILURE:UDP_RECOVERY_PASS");
  return true;
}

void run_gate() {
  char *ssid = nullptr;
  char *password = nullptr;
  IPAddress peer;
  uint16_t tcp_port = 0;
  uint16_t udp_port = 0;
  if (!parse_start(ssid, password, peer, tcp_port, udp_port)) {
    fail("START");
    return;
  }
  WiFi.setTimeout(kWifiTimeoutMs);
  if (WiFi.begin(ssid, password) != WL_CONNECTED && !wait_station()) {
    fail("WIFI");
    return;
  }
  Serial.println("RA4_PEER_FAILURE:WIFI_CONNECTED");
  if (!run_tcp_failure_recovery(peer, tcp_port)) {
    fail("TCP");
    return;
  }
  if (!run_udp_failure_recovery(peer, udp_port)) {
    fail("UDP");
    return;
  }
  WiFi.disconnect();
  Serial.println("RA4_PEER_FAILURE:RESULT=PASS");
  finished = true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("RA4_PEER_FAILURE:READY");
}

void loop() {
  if (finished) {
    delay(10);
    return;
  }
  static unsigned long last_ready = millis();
  if (millis() - last_ready >= 1000) {
    Serial.println("RA4_PEER_FAILURE:READY");
    last_ready = millis();
  }
  while (Serial.available()) {
    const int value = Serial.read();
    if (value == '\r') continue;
    if (value == '\n') {
      command[command_size] = '\0';
      command_size = 0;
      run_gate();
      return;
    }
    if (command_size + 1 >= sizeof(command)) {
      fail("COMMAND");
      return;
    }
    command[command_size++] = static_cast<char>(value);
  }
}
