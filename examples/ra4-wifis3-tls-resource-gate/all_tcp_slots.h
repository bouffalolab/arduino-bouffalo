// Capacity diagnostic: keep 8 plain + 8 accepted + 8 TLS connections alive.
// Plain peers queue 64 bytes each; this is not a 24-stream bulk throughput gate.
int all_tcp_slots[16];
int all_tcp_server = -1;
bool close_all_tcp_slots() {
  bool closed = true;
  for (unsigned i = 0; i < 16; ++i) {
    if (all_tcp_slots[i] < 0) continue;
    std::string response;
    modem.begin();
    if (!modem.write("+CLIENTCLOSE:", response, "AT+CLIENTCLOSE=%d\r\n", all_tcp_slots[i])) closed = false;
    all_tcp_slots[i] = -1;
  }
  if (all_tcp_server >= 0) {
    std::string response;
    modem.begin();
    if (!modem.write("+SERVEREND:", response, "AT+SERVEREND=%d\r\n", all_tcp_server)) closed = false;
    all_tcp_server = -1;
  }
  return closed;
}
bool open_all_tcp_slots(const IPAddress &endpoint, size_t round) {
  const auto fail = [](const char *stage, unsigned index) {
    Serial.print("RA4_TLS_RESOURCE:ALL_TCP_OPEN_FAILURE=");
    Serial.print(stage); Serial.print(","); Serial.println(index);
    return false;
  };
  for (unsigned i = 0; i < 16; ++i) all_tcp_slots[i] = -1;
  std::string response;
  modem.begin();
  if (!modem.write("+BEGINSERVER:", response, "AT+BEGINSERVER=%u\r\n", kMixedServerPort)) return fail("SERVER_BEGIN", 0);
  all_tcp_server = atoi(response.c_str());
  if (all_tcp_server < 0 || all_tcp_server >= 4) return fail("SERVER_SLOT", 0);
  for (unsigned i = 0; i < 8; ++i) {
    response.clear();modem.begin();
    if (!modem.write("+BEGINCLIENT:", response, "AT+BEGINCLIENT\r\n")) return fail("CLIENT_BEGIN", i);
    int slot = atoi(response.c_str());
    if (slot < 0 || slot >= 8) return fail("CLIENT_SLOT", i);
    for (unsigned j = 0; j < i; ++j) if (all_tcp_slots[j] == slot) return fail("DUPLICATE_SLOT", i);
    all_tcp_slots[i] = slot;
    response.clear();modem.begin();
    if (!modem.write("+CLIENTCONNECT:", response, "AT+CLIENTCONNECT=%d,%s,%u,5000\r\n",
        slot, endpoint.toString().c_str(), kMixedTcpPort)) return fail("CLIENT_CONNECT", i);
  }
  for (unsigned i = 8; i < 16; ++i) {
    const unsigned long started = millis();
    bool accepted = false;
    while (millis() - started < 10000UL) {
      response.clear();modem.begin();
      if (modem.write("+SERVERACCEPT:", response, "AT+SERVERACCEPT=%d\r\n", all_tcp_server)) {
        int slot = atoi(response.c_str());
        if (slot < 1000 || slot >= 1008) return fail("ACCEPT_SLOT", i);
        for (unsigned j = 8; j < i; ++j) if (all_tcp_slots[j] == slot) return fail("DUPLICATE_SLOT", i);
        all_tcp_slots[i] = slot;accepted = true;break;
      }
      delay(10);
    }
    if (!accepted) return fail("SERVER_ACCEPT", i);
  }
  // Check all slots at the same phase before asking any peer for application data.
  for (unsigned i = 0; i < 16; ++i) {
    response.clear();modem.begin();
    if (!modem.write("+CLIENTCONNECTED:", response, "AT+CLIENTCONNECTED=%d\r\n", all_tcp_slots[i]) ||
        response != "1") return fail("CLIENT_CONNECTED", i);
  }
  for (unsigned i = 0; i < 8; ++i)
    if (!query_tls_slot_connected(kTlsSocketBase + i, true)) return fail("TLS_CONNECTED", i);
  Serial.print("RA4_TLS_RESOURCE:ALL_TCP_OPEN_PASS=");Serial.print(round);Serial.println(",8,8,8");
  for (unsigned i = 0; i < 16; ++i) {
    response.clear();modem.begin();
    modem.write_nowait("+CLIENTSEND:", response, "AT+CLIENTSEND=%d,1\r\n", all_tcp_slots[i]);
    const uint8_t request = 'X';
    if (!modem.passthrough(&request, 1)) return fail("CLIENT_SEND", i);
  }
  return true;
}
bool verify_all_tcp_slots(size_t round) {
  size_t received[16] = {};
  unsigned completed = 0;
  const unsigned long started = millis();
  while (completed < 16 && millis() - started < 10000UL) {
    for (unsigned i = 0; i < 16; ++i) {
      if (received[i] == 64) continue;
      std::string response;
      modem.begin();
      if (!modem.write("+AVAILABLE:", response, "AT+AVAILABLE=%d\r\n", all_tcp_slots[i])) return false;
      if (atoi(response.c_str()) <= 0) continue;
      response.clear();modem.begin();
      if (!modem.write("+CLIENTRECEIVE:", response, "AT+CLIENTRECEIVE=%d,%u\r\n",
          all_tcp_slots[i], unsigned(64-received[i])) || response.empty() || response.size() > 64-received[i]) return false;
      for (size_t n = 0; n < response.size(); ++n)
        if (uint8_t(response[n]) != uint8_t(0x5aU + (received[i]+n)*19U)) return false;
      received[i] += response.size();
      if (received[i] == 64) ++completed;
    }
    delay(1);
  }
  if (completed != 16) return false;
  Serial.print("RA4_TLS_RESOURCE:ALL_TCP_CONTENT_PASS=");Serial.print(round);Serial.println(",16,64");
  return true;
}
