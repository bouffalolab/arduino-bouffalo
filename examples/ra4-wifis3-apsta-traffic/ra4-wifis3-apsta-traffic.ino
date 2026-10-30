#include <WiFiS3.h>
#include <WiFiUdp.h>
#include <string.h>

namespace {

constexpr uint16_t kTcpPort = 4243;
constexpr uint16_t kUdpPort = 4244;
constexpr size_t kCommandCapacity = 512U;
constexpr size_t kSsidCapacity = 33U;
constexpr size_t kPasswordCapacity = 65U;
constexpr size_t kAtResponseCapacity = 512U;
constexpr unsigned long kAtTimeoutMs = 3000UL;
constexpr unsigned long kConnectTimeoutMs = 30000UL;
/*
 * A failed Windows UDP receive is followed by Windows-only HID diagnostics.
 * Keep the RA4M1 traffic fixture alive long enough for those snapshots to
 * observe the same open WiFiUDP socket rather than its cleanup state.
 */
constexpr unsigned long kTrafficTimeoutMs = 120000UL;

constexpr char kBroadcastProbe[] = "broadcast-probe";
constexpr char kBroadcastReply[] = "broadcast-reply";
constexpr char kAcceptedGreeting[] = "accepted-greeting";
constexpr char kAcceptedAck[] = "accepted-ack";
constexpr char kAcceptedReply[] = "accepted-reply";
constexpr char kUdpRequest[] = "bl616cl-udp-echo";
constexpr char kUdpReply[] = "bl616cl-udp-reply";
constexpr char kRecoveryProbe[] = "recovery-probe";
constexpr char kRecoveryReply[] = "recovery-reply";

enum class Phase : uint8_t {
  WaitStart,
  WaitTrafficStart,
  WaitBroadcastClient,
  WaitAcceptedClient,
  WaitUdp,
  WaitAcceptedAck,
  WaitRecoveryClient,
  Done,
};

WiFiServer server(kTcpPort);
WiFiUDP udp;
WiFiClient broadcast_client;
WiFiClient accepted_client;
WiFiClient recovery_client;
Phase phase = Phase::WaitStart;
char command[kCommandCapacity] = {};
size_t command_size = 0U;
unsigned long phase_started_at = 0U;
char broadcast_received[sizeof(kBroadcastProbe)] = {};
size_t broadcast_received_size = 0U;
char accepted_received[sizeof(kAcceptedAck)] = {};
size_t accepted_received_size = 0U;
char recovery_received[sizeof(kRecoveryProbe)] = {};
size_t recovery_received_size = 0U;
char upstream_ssid[kSsidCapacity] = {};
char upstream_password[kPasswordCapacity] = {};
char ap_ssid[kSsidCapacity] = {};
char ap_password[kPasswordCapacity] = {};
char gateway[16] = {};

void fail(const char* reason) {
  Serial.print("RA4_APSTA:RESULT=FAIL:");
  Serial.println(reason);
  udp.stop();
  broadcast_client.stop();
  accepted_client.stop();
  recovery_client.stop();
  server.end();
  phase = Phase::Done;
}

void stage(const char* name) {
  Serial.print("RA4_APSTA:STAGE=");
  Serial.println(name);
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

bool decode_hex(
    const char* text,
    char* output,
    size_t output_capacity) {
  if (text == nullptr || output == nullptr || output_capacity == 0U) {
    return false;
  }
  const size_t length = strlen(text);
  if ((length & 1U) != 0U || length / 2U >= output_capacity) {
    return false;
  }
  for (size_t index = 0U; index < length; index += 2U) {
    const char high = text[index];
    const char low = text[index + 1U];
    const int high_value =
        high >= '0' && high <= '9' ? high - '0' :
        high >= 'a' && high <= 'f' ? high - 'a' + 10 :
        high >= 'A' && high <= 'F' ? high - 'A' + 10 : -1;
    const int low_value =
        low >= '0' && low <= '9' ? low - '0' :
        low >= 'a' && low <= 'f' ? low - 'a' + 10 :
        low >= 'A' && low <= 'F' ? low - 'A' + 10 : -1;
    if (high_value < 0 || low_value < 0) {
      return false;
    }
    output[index / 2U] = static_cast<char>((high_value << 4) | low_value);
  }
  output[length / 2U] = '\0';
  return output[0] != '\0';
}

bool parse_start() {
  constexpr char kPrefix[] = "START ";
  if (strncmp(command, kPrefix, sizeof(kPrefix) - 1U) != 0) {
    return false;
  }

  char* fields[4] = {};
  char* cursor = command + sizeof(kPrefix) - 1U;
  for (size_t index = 0U; index < 4U; ++index) {
    fields[index] = cursor;
    char* separator = strchr(cursor, index == 3U ? '\0' : ' ');
    if (separator == nullptr) {
      return false;
    }
    if (index != 3U) {
      *separator = '\0';
      cursor = separator + 1U;
      if (*cursor == '\0') {
        return false;
      }
    }
  }
  return decode_hex(fields[0], upstream_ssid, sizeof(upstream_ssid)) &&
      decode_hex(fields[1], upstream_password, sizeof(upstream_password)) &&
      decode_hex(fields[2], ap_ssid, sizeof(ap_ssid)) &&
      decode_hex(fields[3], ap_password, sizeof(ap_password));
}

bool read_at_response(char* response, size_t capacity, unsigned long timeout_ms) {
  if (response == nullptr || capacity < 5U) {
    return false;
  }
  size_t size = 0U;
  const unsigned long deadline = millis() + timeout_ms;
  while (millis() - deadline > 0x80000000UL) {
    while (Serial2.available() > 0) {
      const int value = Serial2.read();
      if (value < 0) {
        continue;
      }
      if (size + 1U >= capacity) {
        return false;
      }
      response[size++] = static_cast<char>(value);
      response[size] = '\0';
      if (size >= 4U && strcmp(response + size - 4U, "OK\r\n") == 0) {
        return true;
      }
    }
    delay(1);
  }
  return false;
}

bool transact_at(const char* request, char* response, size_t response_capacity) {
  while (Serial2.available() > 0) {
    (void)Serial2.read();
  }
  Serial2.print(request);
  Serial2.print("\r\n");
  Serial2.flush();
  return read_at_response(response, response_capacity, kAtTimeoutMs);
}

bool expect_at(const char* request, const char* expected) {
  char response[kAtResponseCapacity] = {};
  return transact_at(request, response, sizeof(response)) &&
      strcmp(response, expected) == 0;
}

bool append_at_argument(
    char* output,
    size_t capacity,
    size_t& size,
    const char* value) {
  if (value == nullptr || size + 2U >= capacity) {
    return false;
  }
  output[size++] = '"';
  for (const char* cursor = value; *cursor != '\0'; ++cursor) {
    if (*cursor == '"' || *cursor == '\\') {
      if (size + 1U >= capacity) {
        return false;
      }
      output[size++] = '\\';
    }
    if (size + 1U >= capacity) {
      return false;
    }
    output[size++] = *cursor;
  }
  if (size + 2U >= capacity) {
    return false;
  }
  output[size++] = '"';
  output[size] = '\0';
  return true;
}

bool begin_sta() {
  char request[256] = "AT+BEGINSTA=";
  size_t size = strlen(request);
  if (!append_at_argument(request, sizeof(request), size, upstream_ssid) ||
      size + 1U >= sizeof(request)) {
    return false;
  }
  request[size++] = ',';
  request[size] = '\0';
  if (!append_at_argument(request, sizeof(request), size, upstream_password)) {
    return false;
  }
  char response[kAtResponseCapacity] = {};
  if (!transact_at(request, response, sizeof(response))) {
    return false;
  }
  return strncmp(response, "+BEGINSTA: ", sizeof("+BEGINSTA: ") - 1U) == 0 &&
      strstr(response, "\r\nOK\r\n") != nullptr;
}

bool begin_softap() {
  char request[256] = "AT+BEGINSOFTAP=";
  size_t size = strlen(request);
  if (!append_at_argument(request, sizeof(request), size, ap_ssid) ||
      size + 1U >= sizeof(request)) {
    return false;
  }
  request[size++] = ',';
  request[size] = '\0';
  if (!append_at_argument(request, sizeof(request), size, ap_password) ||
      size + sizeof(",1,0") >= sizeof(request)) {
    return false;
  }
  strcat(request, ",1,0");
  return expect_at(request, "+BEGINSOFTAP: 1\r\nOK\r\n");
}

bool status_is(uint8_t expected) {
  char response[kAtResponseCapacity] = {};
  if (!transact_at("AT+GETSTATUS?", response, sizeof(response))) {
    return false;
  }
  char expected_response[32] = {};
  snprintf(
      expected_response,
      sizeof(expected_response),
      "+GETSTATUS: %u\r\nOK\r\n",
      static_cast<unsigned int>(expected));
  return strcmp(response, expected_response) == 0;
}

bool wait_status(uint8_t expected, unsigned long timeout_ms) {
  const unsigned long deadline = millis() + timeout_ms;
  while (millis() - deadline > 0x80000000UL) {
    if (status_is(expected)) {
      return true;
    }
    delay(100);
  }
  return false;
}

bool sta_identity_valid() {
  char response[kAtResponseCapacity] = {};
  if (!transact_at("AT+GETSSID?", response, sizeof(response))) {
    return false;
  }
  char expected[96] = {};
  snprintf(expected, sizeof(expected), "+GETSSID: %s\r\nOK\r\n", upstream_ssid);
  if (strcmp(response, expected) != 0 ||
      !transact_at("AT+GETRSSI?", response, sizeof(response))) {
    return false;
  }
  int rssi = 0;
  return sscanf(response, "+GETRSSI: %d\r\nOK\r\n", &rssi) == 1 &&
      rssi >= -127 && rssi <= 0;
}

bool fetch_gateway() {
  char response[kAtResponseCapacity] = {};
  if (!transact_at("AT+IPSTA=1", response, sizeof(response))) {
    return false;
  }
  unsigned int octet[4] = {};
  if (sscanf(
          response,
          "+IPSTA: %u.%u.%u.%u\r\nOK\r\n",
          &octet[0],
          &octet[1],
          &octet[2],
          &octet[3]) != 4 ||
      octet[0] > 255U || octet[1] > 255U ||
      octet[2] > 255U || octet[3] > 255U ||
      (octet[0] == 0U && octet[1] == 0U &&
       octet[2] == 0U && octet[3] == 0U)) {
    return false;
  }
  snprintf(
      gateway,
      sizeof(gateway),
      "%u.%u.%u.%u",
      octet[0],
      octet[1],
      octet[2],
      octet[3]);
  return true;
}

bool ping_gateway() {
  char request[48] = {};
  char response[kAtResponseCapacity] = {};
  snprintf(request, sizeof(request), "AT+PING=0,%s,64,2", gateway);
  int rtt = -1;
  return transact_at(request, response, sizeof(response)) &&
      sscanf(response, "+PING: %d\r\nOK\r\n", &rtt) == 1 &&
      rtt >= 0;
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

void begin_traffic() {
  if (!parse_start()) {
    fail("START");
    return;
  }
  /*
   * This Sketch configures AP+STA through direct AT traffic so it can keep
   * both roles active. Initialize WiFiS3's lazy modem singleton before AP
   * exists: otherwise the first WiFiServer/WiFiUDP call sends its one-time
   * SOFTRESETWIFI after BEGINSOFTAP and intentionally tears the AP down.
  */
  stage("MODEM_BEGIN");
  modem.begin();
  stage("MODE_APSTA");
  if (!expect_at("AT+WIFIMODE=3", "+WIFIMODE: \r\nOK\r\n")) {
    fail("STA");
    return;
  }
  stage("STA_RECONNECT_OFF");
  if (!expect_at("AT+RECONNECT=0", "OK\r\n")) {
    fail("STA");
    return;
  }
  stage("STA_BEGIN");
  if (!begin_sta()) {
    fail("STA");
    return;
  }
  stage("STA_WAIT");
  if (!wait_status(3U, kConnectTimeoutMs)) {
    fail("STA");
    return;
  }
  stage("STA_IDENTITY");
  if (!sta_identity_valid()) {
    fail("STA");
    return;
  }
  stage("STA_GATEWAY");
  if (!fetch_gateway()) {
    fail("STA");
    return;
  }
  stage("STA_PING");
  if (!ping_gateway()) {
    fail("STA");
    return;
  }
  Serial.println("RA4_APSTA:STA_CONNECTED");
  Serial.println("RA4_APSTA:UPSTREAM_PING_BEFORE_AP_PASS");

  stage("SOFTAP_CONFIG");
  if (!expect_at(
          "AT+SOFTAPCONFIG=192.168.42.1,192.168.42.1,255.255.255.0",
          "+SOFTAPCONFIG: \r\nOK\r\n")) {
    fail("SOFTAP");
    return;
  }
  stage("SOFTAP_BEGIN");
  if (!begin_softap()) {
    fail("SOFTAP");
    return;
  }
  stage("SOFTAP_WAIT");
  if (!wait_status(7U, kConnectTimeoutMs)) {
    fail("SOFTAP");
    return;
  }
  stage("SOFTAP_STA_IDENTITY");
  if (!sta_identity_valid()) {
    fail("SOFTAP");
    return;
  }
  stage("SOFTAP_UPSTREAM_PING");
  if (!ping_gateway()) {
    fail("SOFTAP");
    return;
  }
  Serial.println("RA4_APSTA:SOFTAP_READY");
  Serial.println("RA4_APSTA:UPSTREAM_PING_AFTER_AP_PASS");

  stage("SOCKET_BEGIN");
  server.begin();
  if (!server || udp.begin(kUdpPort) != 1) {
    fail("SOCKET_BEGIN");
    return;
  }
  phase = Phase::WaitTrafficStart;
  Serial.println("RA4_APSTA:READY");
}

void service_traffic() {
  if (phase == Phase::WaitBroadcastClient) {
    WiFiClient client = server.available();
    if (client) {
      broadcast_client = client;
      if (!status_is(8U)) {
        fail("AP_CLIENT_STATUS");
        return;
      }
      Serial.println("RA4_APSTA:AP_CLIENT_CONNECTED");
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
        fail("TCP_BROADCAST");
        return;
      }
      phase = Phase::WaitAcceptedClient;
      phase_started_at = millis();
      Serial.println("RA4_APSTA:TCP_BROADCAST_PASS");
    }
  } else if (phase == Phase::WaitAcceptedClient) {
    WiFiClient client = server.accept();
    if (client) {
      accepted_client = client;
      if (accepted_client.write(
              reinterpret_cast<const uint8_t*>(kAcceptedGreeting),
              sizeof(kAcceptedGreeting) - 1U) != sizeof(kAcceptedGreeting) - 1U) {
        fail("TCP_ACCEPT_SEND");
        return;
      }
      /*
       * The two TCP sessions are now concurrently open. Run the UDP
       * request/reply before the second client sends its ACK so this fixture
       * can distinguish UDP polling from the subsequent raw CLIENTSEND
       * command that delivers kAcceptedReply.
       */
      phase = Phase::WaitUdp;
      phase_started_at = millis();
      Serial.println("RA4_APSTA:TCP_ACCEPT_PASS");
    }
  } else if (phase == Phase::WaitUdp) {
    const int packet_size = udp.parsePacket();
    if (packet_size > 0) {
      char request[sizeof(kUdpRequest)] = {};
      if (packet_size != static_cast<int>(sizeof(kUdpRequest) - 1U) ||
          udp.read(request, sizeof(kUdpRequest) - 1U) !=
              static_cast<int>(sizeof(kUdpRequest) - 1U) ||
          memcmp(request, kUdpRequest, sizeof(kUdpRequest) - 1U) != 0) {
        fail("UDP_REQUEST");
        return;
      }
      if (udp.beginPacket(udp.remoteIP(), udp.remotePort()) != 1 ||
          udp.write(
              reinterpret_cast<const uint8_t*>(kUdpReply),
              sizeof(kUdpReply) - 1U) != sizeof(kUdpReply) - 1U ||
          udp.endPacket() != 1) {
        fail("UDP_REPLY");
        return;
      }
      Serial.println("RA4_APSTA:UDP_RESULT=PASS");
      phase = Phase::WaitAcceptedAck;
      phase_started_at = millis();
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
        fail("TCP_ACCEPT_REPLY");
        return;
      }
      if (!sta_identity_valid() || !ping_gateway()) {
        fail("UPSTREAM_AFTER_TRAFFIC");
        return;
      }
      udp.stop();
      broadcast_client.stop();
      accepted_client.stop();
      phase = Phase::WaitRecoveryClient;
      phase_started_at = millis();
      Serial.println("RA4_APSTA:UPSTREAM_PING_AFTER_TRAFFIC_PASS");
      Serial.println("RA4_APSTA:PRIMARY_TRAFFIC_PASS");
    }
  } else if (phase == Phase::WaitRecoveryClient) {
    if (!recovery_client) {
      recovery_client = server.accept();
    }
    if (recovery_client && read_expected(
            recovery_client,
            kRecoveryProbe,
            recovery_received,
            recovery_received_size,
            sizeof(recovery_received))) {
      if (recovery_client.write(
              reinterpret_cast<const uint8_t*>(kRecoveryReply),
              sizeof(kRecoveryReply) - 1U) != sizeof(kRecoveryReply) - 1U ||
          !sta_identity_valid() || !ping_gateway()) {
        recovery_client.stop();
        fail("AP_REASSOCIATION");
        return;
      }
      recovery_client.stop();
      server.end();
      phase = Phase::Done;
      Serial.println("RA4_APSTA:AP_REASSOCIATION_TRAFFIC_PASS");
      Serial.println("RA4_APSTA:UPSTREAM_PING_AFTER_REJOIN_PASS");
      Serial.println("RA4_APSTA:RESULT=PASS");
    }
  }

  if (phase != Phase::WaitStart &&
      phase != Phase::WaitTrafficStart &&
      phase != Phase::Done &&
      millis() - phase_started_at > kTrafficTimeoutMs) {
    fail("TIMEOUT");
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200);
  Serial.println("RA4_APSTA:BOOT");
  Serial.println("RA4_APSTA:WAIT_START");
}

void loop() {
  if (read_command_line()) {
    if (phase == Phase::WaitStart) {
      begin_traffic();
    } else if (phase == Phase::WaitTrafficStart) {
      if (strcmp(command, "TRAFFIC") != 0) {
        fail("TRAFFIC");
      } else {
        phase = Phase::WaitBroadcastClient;
        phase_started_at = millis();
        Serial.println("RA4_APSTA:TRAFFIC_START");
      }
    }
  }
  service_traffic();
  delay(1);
}
