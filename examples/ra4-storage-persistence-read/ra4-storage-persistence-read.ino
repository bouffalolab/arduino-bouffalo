/*
 * Read and cleanup phase for the BL616CL storage persistence gate.
 *
 * This fixture is uploaded only after BL616CL has been reset without
 * rewriting its Flash. It validates the data written by the write phase.
 */

namespace {

constexpr unsigned long kBaud = 115200;
constexpr size_t kResponseCapacity = 512;
uint8_t response[kResponseCapacity];
size_t response_size = 0;

void fail(const char *stage)
{
  Serial.print("RA4_STORAGE_PERSIST_READ:FAIL:");
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
    const uint8_t *expected,
    size_t expected_size)
{
  while (Serial2.available() > 0) {
    (void)Serial2.read();
  }
  Serial2.print(command);
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
  Serial.println("RA4_STORAGE_PERSIST_READ:START");

  static const uint8_t mount[] = "+MOUNTFS: FS Mounted\r\n\r\nOK\r\n";
  require("mount", "AT+MOUNTFS=0\r\n", mount, sizeof(mount) - 1);

  static const uint8_t file[] =
      {'+', 'F', 'S', ':', ' ', '9', '|',
       0x00, 'p', 'e', 'r', 's', 'i', 's', 't', 0xff,
       '\r', '\n', 'O', 'K', '\r', '\n'};
  require(
      "file-read",
      "AT+FS=0,2,/codex-persist.bin,0,32\r\n",
      file,
      sizeof(file));

  static const uint8_t pref_begin_read_only[] =
      "+PREFBEGIN: 1\r\n\r\nOK\r\n";
  require(
      "pref-begin-read-only",
      "AT+PREFBEGIN=codexpersist,1,\r\n",
      pref_begin_read_only,
      sizeof(pref_begin_read_only) - 1);
  static const uint8_t pref_get_i32[] = "+PREFGET: -24680\r\nOK\r\n";
  require(
      "pref-get-i32",
      "AT+PREFGET=marker,4,0\r\n",
      pref_get_i32,
      sizeof(pref_get_i32) - 1);
  static const uint8_t pref_get_blob[] =
      {'+', 'P', 'R', 'E', 'F', 'G', 'E', 'T', ':', ' ', '4', '|',
       0x61, 0x00, 0x62, 0xff, '\r', '\n', 'O', 'K', '\r', '\n'};
  require(
      "pref-get-blob",
      "AT+PREFGET=blob,9\r\n",
      pref_get_blob,
      sizeof(pref_get_blob));

  static const uint8_t pref_type_missing[] = "+PREFTYPE: 10\r\nOK\r\n";
  require(
      "pref-type-missing",
      "AT+PREFTYPE=missing\r\n",
      pref_type_missing,
      sizeof(pref_type_missing) - 1);
  static const uint8_t pref_len_missing[] = "+PREFLEN: 0\r\nOK\r\n";
  require(
      "pref-len-missing",
      "AT+PREFLEN=missing\r\n",
      pref_len_missing,
      sizeof(pref_len_missing) - 1);
  static const uint8_t pref_get_missing_i32[] = "+PREFGET: -7\r\nOK\r\n";
  require(
      "pref-get-missing-i32",
      "AT+PREFGET=missing,4,-7\r\n",
      pref_get_missing_i32,
      sizeof(pref_get_missing_i32) - 1);
  static const uint8_t pref_get_missing_string[] =
      "+PREFGET: 8|fallback\r\nOK\r\n";
  require(
      "pref-get-missing-string",
      "AT+PREFGET=missingtext,8,fallback\r\n",
      pref_get_missing_string,
      sizeof(pref_get_missing_string) - 1);
  static const uint8_t pref_get_missing_blob[] =
      "+PREFGET: 0|\r\nOK\r\n";
  require(
      "pref-get-missing-blob",
      "AT+PREFGET=missingblob,9\r\n",
      pref_get_missing_blob,
      sizeof(pref_get_missing_blob) - 1);
  static const uint8_t pref_put_read_only[] = "+PREFPUT: 0\r\nOK\r\n";
  require(
      "pref-put-read-only",
      "AT+PREFPUT=marker,4,1\r\n",
      pref_put_read_only,
      sizeof(pref_put_read_only) - 1);
  static const uint8_t pref_clear_read_only[] = "+PREFCLEAR: 0\r\nOK\r\n";
  require(
      "pref-clear-read-only",
      "AT+PREFCLEAR\r\n",
      pref_clear_read_only,
      sizeof(pref_clear_read_only) - 1);
  static const uint8_t pref_end[] = "+PREFEND: 0\r\n\r\nOK\r\n";
  require("pref-end-read-only", "AT+PREFEND\r\n", pref_end, sizeof(pref_end) - 1);

  static const uint8_t pref_begin_write[] = "+PREFBEGIN: 1\r\n\r\nOK\r\n";
  require(
      "pref-begin-write",
      "AT+PREFBEGIN=codexpersist,0,\r\n",
      pref_begin_write,
      sizeof(pref_begin_write) - 1);
  static const uint8_t pref_remove_missing[] = "+PREFREMOVE: 0\r\nOK\r\n";
  require(
      "pref-remove-missing",
      "AT+PREFREMOVE=missing\r\n",
      pref_remove_missing,
      sizeof(pref_remove_missing) - 1);
  static const uint8_t pref_clear[] = "+PREFCLEAR: 1\r\nOK\r\n";
  require("pref-clear", "AT+PREFCLEAR\r\n", pref_clear, sizeof(pref_clear) - 1);
  require("pref-end-write", "AT+PREFEND\r\n", pref_end, sizeof(pref_end) - 1);

  static const uint8_t ok[] = "OK\r\n";
  require(
      "file-delete",
      "AT+FS=0,0,/codex-persist.bin\r\n",
      ok,
      sizeof(ok) - 1);
  Serial.println("RA4_STORAGE_PERSIST_READ:PASS");
}

void loop()
{
  delay(1000);
}
