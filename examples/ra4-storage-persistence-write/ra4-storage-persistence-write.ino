/*
 * Write phase for the BL616CL storage persistence gate.
 *
 * The host resets only BL616CL after this sketch reports WRITE_PASS. A
 * separate RA4M1 read fixture then proves that LittleFS and EasyFlash data
 * survived the bridge reset without a BL616CL Flash rewrite.
 */

namespace {

constexpr unsigned long kBaud = 115200;
constexpr size_t kResponseCapacity = 512;
uint8_t response[kResponseCapacity];
size_t response_size = 0;

void fail(const char *stage)
{
  Serial.print("RA4_STORAGE_PERSIST_WRITE:FAIL:");
  Serial.println(stage);
  delay(1000);
  NVIC_SystemReset();
}

bool read_response(const uint8_t *expected, size_t expected_size)
{
  size_t size = 0;
  const unsigned long deadline = millis() + 5000;
  while (static_cast<long>(millis() - deadline) < 0) {
    while (Serial2.available() > 0) {
      const int value = Serial2.read();
      if (size < sizeof(response)) {
        response[size++] = static_cast<uint8_t>(value);
      }
      if (size >= 4 &&
          response[size - 4] == 'O' &&
          response[size - 3] == 'K' &&
          response[size - 2] == '\r' &&
          response[size - 1] == '\n') {
        response_size = size;
        return size == expected_size &&
            memcmp(response, expected, expected_size) == 0;
      }
      if (size >= 7 &&
          response[size - 7] == 'E' &&
          response[size - 6] == 'R' &&
          response[size - 5] == 'R' &&
          response[size - 4] == 'O' &&
          response[size - 3] == 'R' &&
          response[size - 2] == '\r' &&
          response[size - 1] == '\n') {
        response_size = size;
        return false;
      }
    }
    delay(1);
  }
  response_size = 0;
  return false;
}

void require(
    const char *stage,
    const char *command,
    const uint8_t *payload,
    size_t payload_size,
    const uint8_t *expected,
    size_t expected_size)
{
  while (Serial2.available() > 0) {
    (void)Serial2.read();
  }
  Serial2.print(command);
  if (payload_size != 0) {
    Serial2.write(payload, payload_size);
  }
  Serial2.flush();
  if (!read_response(expected, expected_size)) {
    fail(stage);
  }
}

}  // namespace

void setup()
{
  Serial.begin(kBaud);
  Serial2.begin(kBaud);
  delay(5000);
  Serial.println("RA4_STORAGE_PERSIST_WRITE:START");

  static const uint8_t mount[] = "+MOUNTFS: FS Mounted\r\n\r\nOK\r\n";
  require("mount", "AT+MOUNTFS=1\r\n", nullptr, 0, mount, sizeof(mount) - 1);

  static const uint8_t file_payload[] = {0x00, 'p', 'e', 'r', 's', 'i', 's', 't', 0xff};
  static const uint8_t ok[] = "OK\r\n";
  require(
      "file-write",
      "AT+FS=0,1,/codex-persist.bin,9\r\n",
      file_payload,
      sizeof(file_payload),
      ok,
      sizeof(ok) - 1);

  static const uint8_t pref_begin[] = "+PREFBEGIN: 1\r\n\r\nOK\r\n";
  require(
      "pref-begin",
      "AT+PREFBEGIN=codexpersist,0,\r\n",
      nullptr,
      0,
      pref_begin,
      sizeof(pref_begin) - 1);
  static const uint8_t pref_put_i32[] = "+PREFPUT: 4\r\nOK\r\n";
  require(
      "pref-put-i32",
      "AT+PREFPUT=marker,4,-24680\r\n",
      nullptr,
      0,
      pref_put_i32,
      sizeof(pref_put_i32) - 1);

  static const uint8_t blob_payload[] = {0x61, 0x00, 0x62, 0xff};
  static const uint8_t pref_put_blob[] = "+PREFPUT: 4\r\nOK\r\n";
  require(
      "pref-put-blob",
      "AT+PREFPUT=blob,9,4\r\n",
      blob_payload,
      sizeof(blob_payload),
      pref_put_blob,
      sizeof(pref_put_blob) - 1);

  static const uint8_t pref_end[] = "+PREFEND: 0\r\n\r\nOK\r\n";
  require(
      "pref-end",
      "AT+PREFEND\r\n",
      nullptr,
      0,
      pref_end,
      sizeof(pref_end) - 1);
  Serial.println("RA4_STORAGE_PERSIST_WRITE:PASS");
}

void loop()
{
  delay(1000);
}
