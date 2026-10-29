#include <WiFiS3.h>
#include <Modem.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#ifndef UDP_LARGE_DATAGRAMS
#define UDP_LARGE_DATAGRAMS 1
#endif
static_assert(UDP_LARGE_DATAGRAMS >= 1 && UDP_LARGE_DATAGRAMS <= 20,
              "large UDP count must be 1..20");

namespace {

constexpr size_t kSlots = 4;
constexpr size_t kRounds = 3;
constexpr size_t kStressCycles = 3;
constexpr size_t kMiddle = 1;
constexpr unsigned long kWaitMs = 8000;
char command[224] = {};
size_t command_size = 0;
bool finished = false;
WiFiUDP sockets[kSlots];
WiFiUDP extra;
WiFiUDP reused;

void fail(const char *reason) {
  Serial.print("RA4_UDP_RESOURCE:RESULT=FAIL:");
  Serial.println(reason);
  finished = true;
}

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool decode(char *field) {
  size_t count = strlen(field);
  if (count == 0 || (count & 1U)) return false;
  for (size_t i = 0; i < count / 2; ++i) {
    int hi = hex_digit(field[2 * i]);
    int lo = hex_digit(field[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    field[i] = static_cast<char>((hi << 4) | lo);
  }
  field[count / 2] = '\0';
  return true;
}

bool parse_start(char *&ssid, char *&password, IPAddress &peer, uint16_t &port,
                 bool &stress) {
  if (strncmp(command, "START ", 6) != 0) return false;
  char *fields[4] = {command + 6};
  for (size_t i = 1; i < 4; ++i) {
    fields[i] = strchr(fields[i - 1], ' ');
    if (!fields[i]) return false;
    *fields[i]++ = '\0';
  }
  char *mode = strchr(fields[3], ' ');
  if (mode) *mode++ = '\0';
  if (mode && strcmp(mode, "STRESS") != 0) return false;
  stress = mode != nullptr;
  char *end = nullptr;
  unsigned long value = strtoul(fields[3], &end, 10);
  ssid = fields[0];
  password = fields[1];
  if (!decode(ssid) || !decode(password) || !peer.fromString(fields[2]) ||
      *end || value < 1 || value > 65534) return false;
  port = static_cast<uint16_t>(value);
  return true;
}

bool exchange(WiFiUDP &udp, IPAddress peer, uint16_t port, unsigned int round,
              unsigned int slot) {
  char request[48] = {};
  int length = snprintf(request, sizeof(request),
                        "bl616cl-udp-resource-r%u-s%u", round, slot);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(request) ||
      udp.beginPacket(peer, port) != 1 ||
      udp.write(reinterpret_cast<const uint8_t *>(request), length) !=
          static_cast<size_t>(length) || udp.endPacket() != 1) return false;

  const unsigned long start = millis();
  int available = 0;
  while (millis() - start < kWaitMs) {
    available = udp.parsePacket();
    if (available > 0) break;
    delay(20);
  }
  if (available != length || udp.remoteIP() != peer ||
      udp.remotePort() != port || udp.available() != length ||
      udp.peek() != request[0]) return false;
  char reply[sizeof(request)] = {};
  if (udp.read(reinterpret_cast<unsigned char *>(reply), length) != length ||
      memcmp(reply, request, length) != 0 || udp.available() != 0) return false;
  return true;
}

bool overlap_max_datagram(WiFiUDP &udp, IPAddress peer, uint16_t port, unsigned int sequence) {
  constexpr char probe[] = "bl616cl-udp-overlap-probe";
  if (udp.beginPacket(peer, port) != 1 ||
      udp.write(reinterpret_cast<const uint8_t *>(probe), sizeof(probe) - 1U) !=
          sizeof(probe) - 1U || udp.endPacket() != 1) return false;
  const unsigned long start = millis();
  int pending = 0;
  while (millis() - start < kWaitMs) {
    pending = udp.parsePacket();
    if (pending > 0) break;
    delay(20);
  }
  if (pending != static_cast<int>(sizeof(probe) - 1U)) return false;
  Serial.println("RA4_UDP_RESOURCE:OVERLAP_RECEIVE_PENDING_PASS");

  uint8_t chunk[256];
#if UDP_VARIED_PAYLOAD
  for (size_t index = 0; index < sizeof(chunk); ++index) {
    chunk[index] = static_cast<uint8_t>(sequence + index * 19U);
  }
#else
  memset(chunk, sequence, sizeof(chunk));
#endif
  if (udp.beginPacket(peer, port) != 1) return false;
  Serial.println("RA4_UDP_RESOURCE:OVERLAP_BEGIN_PACKET_PASS");
  for (size_t offset = 0; offset < 16384U; offset += sizeof(chunk)) {
    if (udp.write(chunk, sizeof(chunk)) != sizeof(chunk)) {
      Serial.print("RA4_UDP_RESOURCE:OVERLAP_WRITE_FAILURE_OFFSET=");
      Serial.println(offset);
      return false;
    }
  }
  Serial.println("RA4_UDP_RESOURCE:OVERLAP_WRITE_PASS");
  if (udp.endPacket() != 1) {
    std::string diagnostic;
    modem.begin();
    if (modem.write(
            "+UDPDEBUG:", diagnostic, "AT+UDPDEBUG=0\r\n")) {
      Serial.print("RA4_UDP_RESOURCE:OVERLAP_END_DIAG=");
      Serial.println(diagnostic.c_str());
    } else {
      Serial.println("RA4_UDP_RESOURCE:OVERLAP_END_DIAG=UNAVAILABLE");
    }
    return false;
  }
  Serial.println("RA4_UDP_RESOURCE:OVERLAP_16K_SEND_PASS");

  char received[sizeof(probe)] = {};
  if (udp.read(reinterpret_cast<unsigned char *>(received), sizeof(probe) - 1U) !=
          static_cast<int>(sizeof(probe) - 1U) ||
      memcmp(received, probe, sizeof(probe) - 1U) != 0) return false;
  Serial.println("RA4_UDP_RESOURCE:OVERLAP_RECEIVE_READ_PASS");
  // endPacket acknowledges queueing, not delivery. Keep STA connected until
  // the peer confirms the entire fragmented datagram before stop_all().
  char receipt[48] = {};
  if (sequence == 0) strcpy(receipt, "bl616cl-udp-16k-received");
  else snprintf(receipt, sizeof(receipt), "bl616cl-udp-16k-received-%u", sequence);
  const size_t receipt_length = strlen(receipt);
  const unsigned long receipt_start = millis();
  int receipt_size = 0;
  while (millis() - receipt_start < kWaitMs) {
    receipt_size = udp.parsePacket();
    if (receipt_size > 0) break;
    delay(20);
  }
  char receipt_data[sizeof(receipt)] = {};
  if (receipt_size != static_cast<int>(receipt_length) ||
      udp.remoteIP() != peer || udp.remotePort() != port ||
      udp.read(reinterpret_cast<unsigned char *>(receipt_data),
          receipt_length) != static_cast<int>(receipt_length) ||
      memcmp(receipt_data, receipt, receipt_length) != 0) return false;
  Serial.println("RA4_UDP_RESOURCE:OVERLAP_PEER_ACK_PASS");
  return true;
}

#if UDP_FAILURE_CONNECTIVITY_PROBE
// Diagnostic only: one small exchange on the same socket after failure. Never
// retry the failed datagram or turn a missing large receipt into a PASS.
bool probe_after_large_failure(WiFiUDP &udp, IPAddress peer, uint16_t port) {
  constexpr char request[] = "bl616cl-udp-post-failure-probe";
  constexpr size_t length = sizeof(request) - 1U;
  if (udp.beginPacket(peer, port) != 1 ||
      udp.write(reinterpret_cast<const uint8_t *>(request), length) != length ||
      udp.endPacket() != 1) return false;
  const unsigned long start = millis();
  while (millis() - start < kWaitMs) {
    int available = udp.parsePacket();
    if (available > 0) {
      const bool expected_peer = udp.remoteIP() == peer && udp.remotePort() == port;
      char reply[sizeof(request)] = {};
      const int count = udp.read(reinterpret_cast<unsigned char *>(reply), length);
      udp.flush();
      if (expected_peer && available == static_cast<int>(length) &&
          count == static_cast<int>(length) && memcmp(reply, request, length) == 0)
        return true;
    }
    delay(20);
  }
  return false;
}
#endif

void stop_all() {
  for (auto &udp : sockets) udp.stop();
  extra.stop();
  reused.stop();
  WiFi.disconnect();
}

bool connect_station(const char *ssid, const char *password) {
  if (WiFi.begin(ssid, password) != WL_CONNECTED) {
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 30000UL) delay(20);
    if (WiFi.status() != WL_CONNECTED) return false;
  }
  return true;
}

bool run_cycle(IPAddress peer, uint16_t port, size_t cycle) {
  const size_t sequence = cycle * (kRounds + 1);
  Serial.println("RA4_UDP_RESOURCE:WIFI_CONNECTED");
  constexpr uint16_t kBasePort = 25020;
  for (size_t i = 0; i < kSlots; ++i) {
    if (sockets[i].begin(kBasePort + i) != 1) {
      fail("OPEN");
      return false;
    }
  }
  Serial.println("RA4_UDP_RESOURCE:FOUR_OPEN_PASS");
  // An invalid local port and an exhausted fifth slot must not acquire a socket.
  if (extra.begin(0) != 0 || extra.begin(kBasePort + kSlots) != 0) {
    fail("REJECT");
    return false;
  }
  Serial.println("RA4_UDP_RESOURCE:REJECT_PASS");
  for (size_t round = 0; round < kRounds; ++round) {
    for (size_t slot = 0; slot < kSlots; ++slot) {
      if (!exchange(sockets[slot], peer, port, sequence + round, slot)) {
        fail("EXCHANGE");
        return false;
      }
    }
    Serial.print("RA4_UDP_RESOURCE:ROUND_PASS=");
    Serial.println(round + 1);
  }
  sockets[kMiddle].stop();
  if (reused.begin(kBasePort + kMiddle) != 1 ||
      !exchange(reused, peer, port, sequence + kRounds, kMiddle)) {
    fail("REBIND");
    return false;
  }
  Serial.println("RA4_UDP_RESOURCE:REBIND_PASS");
  return true;
}

void run_gate() {
  char *ssid = nullptr;
  char *password = nullptr;
  IPAddress peer;
  uint16_t port = 0;
  bool stress = false;
  if (!parse_start(ssid, password, peer, port, stress)) {
    fail("START");
    return;
  }
  const unsigned long started_at = millis();
  const size_t cycles = stress ? kStressCycles : 1;
  for (size_t cycle = 0; cycle < cycles; ++cycle) {
    if (!connect_station(ssid, password)) {
      stop_all();
      fail("WIFI");
      return;
    }
    if (!run_cycle(peer, port, cycle)) {
      stop_all();
      return;
    }
    if (cycle + 1U == cycles) {
      for (unsigned int sequence = 0; sequence < UDP_LARGE_DATAGRAMS; ++sequence) {
        if (!overlap_max_datagram(sockets[0], peer, port, sequence)) {
          Serial.print("RA4_UDP_RESOURCE:LARGE_DATAGRAM_FAILURE=");
          Serial.println(sequence + 1U);
          std::string diagnostic;
          modem.begin();
          if (modem.write("+UDPDEBUG:", diagnostic, "AT+UDPDEBUG=0\r\n")) {
            Serial.print("RA4_UDP_RESOURCE:LARGE_DATAGRAM_FAILURE_DIAG=");
            Serial.println(diagnostic.c_str());
          } else {
            Serial.println("RA4_UDP_RESOURCE:LARGE_DATAGRAM_FAILURE_DIAG=UNAVAILABLE");
          }
#if UDP_FAILURE_CONNECTIVITY_PROBE
          Serial.print("RA4_UDP_RESOURCE:POST_FAILURE_SMALL_ECHO=");
          Serial.println(probe_after_large_failure(sockets[0], peer, port) ? "PASS" : "FAIL");
#endif
          stop_all();
          fail("OVERLAP");
          return;
        }
#if UDP_FRAGMENT_DIAGNOSTICS
        std::string diagnostic;
        modem.begin();
        if (modem.write("+UDPDEBUG:", diagnostic, "AT+UDPDEBUG=0\r\n")) {
          Serial.print("RA4_UDP_RESOURCE:LARGE_DATAGRAM_CONTENT_DIAG=");
          Serial.print(sequence + 1U);
          Serial.print(":");
          Serial.println(diagnostic.c_str());
        } else {
          stop_all();
          fail("FRAGMENT_DIAGNOSTICS");
          return;
        }
#endif
        Serial.print("RA4_UDP_RESOURCE:LARGE_DATAGRAM_PASS=");
        Serial.println(sequence + 1U);
      }
    }
    stop_all();
    if (stress) {
      unsigned long disconnected_at = millis();
      while (WiFi.status() == WL_CONNECTED &&
             millis() - disconnected_at < 10000UL) delay(20);
      if (WiFi.status() == WL_CONNECTED) {
        fail("DISCONNECT");
        return;
      }
      Serial.print("RA4_UDP_RESOURCE:CYCLE_PASS=");
      Serial.println(cycle + 1);
    }
  }
  if (stress) {
    Serial.print("RA4_UDP_RESOURCE:ELAPSED_MS=");
    Serial.println(millis() - started_at);
  }
  Serial.println("RA4_UDP_RESOURCE:RESULT=PASS");
  finished = true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("RA4_UDP_RESOURCE:READY");
}

void loop() {
  if (finished) {
    delay(10);
    return;
  }
  static unsigned long last_ready = millis();
  if (millis() - last_ready >= 1000) {
    Serial.println("RA4_UDP_RESOURCE:READY");
    last_ready = millis();
  }
  while (Serial.available()) {
    int value = Serial.read();
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
