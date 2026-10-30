#include <WiFiS3.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr size_t kPayloadBytes = 1000;
constexpr size_t kTcpChunks = 64;
constexpr size_t kUdpPackets = 32;
constexpr unsigned long kWifiTimeoutMs = 30000;
constexpr unsigned long kIoTimeoutMs = 15000;
char command[240] = {};
size_t command_size = 0;
bool finished = false;
uint8_t transmit_buffer[kPayloadBytes] = {};
uint8_t receive_buffer[kPayloadBytes] = {};
WiFiClient tcp;
WiFiUDP udp;

void fail(const char *reason) {
  tcp.stop();
  udp.stop();
  WiFi.disconnect();
  Serial.print("RA4_NETWORK_THROUGHPUT:RESULT=FAIL:");
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
      parsed_tcp < 1 || parsed_tcp > 65535 ||
      parsed_udp < 1 || parsed_udp > 65535) return false;
  tcp_port = static_cast<uint16_t>(parsed_tcp);
  udp_port = static_cast<uint16_t>(parsed_udp);
  return true;
}

void fill_payload(uint32_t sequence) {
  transmit_buffer[0] = static_cast<uint8_t>(sequence >> 24U);
  transmit_buffer[1] = static_cast<uint8_t>(sequence >> 16U);
  transmit_buffer[2] = static_cast<uint8_t>(sequence >> 8U);
  transmit_buffer[3] = static_cast<uint8_t>(sequence);
  for (size_t index = 4; index < kPayloadBytes; ++index) {
    transmit_buffer[index] = static_cast<uint8_t>(
        sequence * 31U + static_cast<uint32_t>(index) * 17U);
  }
}

bool wait_station() {
  const unsigned long started_at = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - started_at >= kWifiTimeoutMs) return false;
    delay(20);
  }
  return true;
}

bool read_tcp_exact(size_t expected) {
  size_t received = 0;
  const unsigned long started_at = millis();
  while (received < expected && millis() - started_at < kIoTimeoutMs) {
    const int count = tcp.read(receive_buffer + received, expected - received);
    if (count > 0) {
      received += static_cast<size_t>(count);
    } else {
      delay(2);
    }
  }
  return received == expected;
}

bool run_tcp(IPAddress peer, uint16_t port, unsigned long &elapsed) {
  tcp.setConnectionTimeout(10000);
  if (tcp.connect(peer, port) != 1) return false;
  const unsigned long started_at = millis();
  for (size_t chunk = 0; chunk < kTcpChunks; ++chunk) {
    fill_payload(static_cast<uint32_t>(chunk));
    if (tcp.write(transmit_buffer, sizeof(transmit_buffer)) !=
        sizeof(transmit_buffer)) return false;
    memset(receive_buffer, 0, sizeof(receive_buffer));
    if (!read_tcp_exact(sizeof(receive_buffer)) ||
        memcmp(receive_buffer, transmit_buffer, sizeof(receive_buffer)) != 0) {
      return false;
    }
  }
  elapsed = millis() - started_at;
  tcp.stop();
  return true;
}

bool run_udp(IPAddress peer, uint16_t port, unsigned long &elapsed) {
  if (udp.begin(25200) != 1) return false;
  const unsigned long started_at = millis();
  for (size_t packet = 0; packet < kUdpPackets; ++packet) {
    const uint32_t sequence = 0x80000000UL + static_cast<uint32_t>(packet);
    fill_payload(sequence);
    if (udp.beginPacket(peer, port) != 1 ||
        udp.write(transmit_buffer, sizeof(transmit_buffer)) !=
            sizeof(transmit_buffer) ||
        udp.endPacket() != 1) return false;
    const unsigned long packet_started_at = millis();
    int packet_size = 0;
    while (millis() - packet_started_at < kIoTimeoutMs) {
      packet_size = udp.parsePacket();
      if (packet_size > 0) break;
      delay(2);
    }
    if (packet_size != static_cast<int>(sizeof(receive_buffer)) ||
        udp.remoteIP() != peer || udp.remotePort() != port ||
        udp.read(receive_buffer, sizeof(receive_buffer)) !=
            static_cast<int>(sizeof(receive_buffer)) ||
        memcmp(receive_buffer, transmit_buffer, sizeof(receive_buffer)) != 0) {
      return false;
    }
  }
  elapsed = millis() - started_at;
  udp.stop();
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
  Serial.println("RA4_NETWORK_THROUGHPUT:WIFI_CONNECTED");

  unsigned long tcp_elapsed = 0;
  if (!run_tcp(peer, tcp_port, tcp_elapsed)) {
    fail("TCP");
    return;
  }
  Serial.print("RA4_NETWORK_THROUGHPUT:TCP_BYTES=");
  Serial.println(kPayloadBytes * kTcpChunks);
  Serial.print("RA4_NETWORK_THROUGHPUT:TCP_CHUNKS=");
  Serial.println(kTcpChunks);
  Serial.print("RA4_NETWORK_THROUGHPUT:TCP_ELAPSED_MS=");
  Serial.println(tcp_elapsed);

  unsigned long udp_elapsed = 0;
  if (!run_udp(peer, udp_port, udp_elapsed)) {
    fail("UDP");
    return;
  }
  Serial.print("RA4_NETWORK_THROUGHPUT:UDP_BYTES=");
  Serial.println(kPayloadBytes * kUdpPackets);
  Serial.print("RA4_NETWORK_THROUGHPUT:UDP_PACKETS=");
  Serial.println(kUdpPackets);
  Serial.print("RA4_NETWORK_THROUGHPUT:UDP_ELAPSED_MS=");
  Serial.println(udp_elapsed);

  WiFi.disconnect();
  Serial.println("RA4_NETWORK_THROUGHPUT:RESULT=PASS");
  finished = true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("RA4_NETWORK_THROUGHPUT:READY");
}

void loop() {
  if (finished) {
    delay(10);
    return;
  }
  static unsigned long last_ready = millis();
  if (millis() - last_ready >= 1000) {
    Serial.println("RA4_NETWORK_THROUGHPUT:READY");
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
