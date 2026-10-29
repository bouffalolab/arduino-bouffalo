/*
 * Temporary storage compatibility fixture for the BL616CL bridge.
 *
 * Serial is the RA4M1 user UART exposed through the bridge CDC. Serial2 is the
 * private AT UART connected to the bridge. The fixture sends the same raw
 * frames used by WiFiS3's filesystem and Preferences libraries.
 */

namespace {

constexpr unsigned long kBaud = 115200;
constexpr size_t kResponseCapacity = 8192;
uint8_t response[kResponseCapacity];
size_t response_size = 0;

void fail(const char *stage)
{
  Serial.print("RA4_STORAGE:FAIL:");
  Serial.println(stage);
  Serial.print("RA4_STORAGE:ACTUAL_HEX:");
  for (size_t index = 0; index < response_size; ++index) {
    if (response[index] < 16) {
      Serial.print('0');
    }
    Serial.print(response[index], HEX);
  }
  Serial.println();
  delay(1000);
  NVIC_SystemReset();
}

size_t read_response()
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
        return size;
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
        return size;
      }
    }
    delay(1);
  }
  response_size = 0;
  return 0;
}

bool response_equals(const uint8_t *expected, size_t expected_size)
{
  const size_t actual_size = read_response();
  return actual_size == expected_size &&
      memcmp(response, expected, expected_size) == 0;
}

bool transact(
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
  return response_equals(expected, expected_size);
}

void require(
    const char *stage,
    const char *command,
    const uint8_t *payload,
    size_t payload_size,
    const uint8_t *expected,
    size_t expected_size)
{
  if (!transact(command, payload, payload_size, expected, expected_size)) {
    fail(stage);
  }
}

uint8_t pattern_byte(size_t offset)
{
  return static_cast<uint8_t>((offset * 37U + offset / 251U) & 0xffU);
}

void write_pattern(const char *stage, const char *command, size_t size,
                   const char *expected)
{
  uint8_t chunk[256];
  while (Serial2.available() > 0) {
    (void)Serial2.read();
  }
  Serial2.print(command);
  for (size_t offset = 0; offset < size;) {
    const size_t count = size - offset < sizeof(chunk)
        ? size - offset : sizeof(chunk);
    for (size_t index = 0; index < count; ++index) {
      chunk[index] = pattern_byte(offset + index);
    }
    Serial2.write(chunk, count);
    offset += count;
  }
  Serial2.flush();
  if (!response_equals(reinterpret_cast<const uint8_t *>(expected),
                       strlen(expected))) {
    fail(stage);
  }
}

void read_pattern(const char *stage, const char *command, const char *prefix,
                  size_t offset, size_t size)
{
  // Check framing and payload as a stream: no second 16 KiB RA4M1 buffer,
  // and binary payload containing OK/ERROR never terminates the read early.
  static const char tail[] = "\r\nOK\r\n";
  const size_t prefix_size = strlen(prefix);
  const size_t total = prefix_size + size + sizeof(tail) - 1;
  Serial2.print(command);
  Serial2.flush();
  response_size = 0;
  const unsigned long deadline = millis() + 10000;
  for (size_t index = 0; index < total; ++index) {
    while (Serial2.available() == 0) {
      if (static_cast<long>(millis() - deadline) >= 0) {
        fail(stage);
      }
      delay(1);
    }
    const uint8_t actual = static_cast<uint8_t>(Serial2.read());
    const uint8_t expected = index < prefix_size
        ? static_cast<uint8_t>(prefix[index])
        : index < prefix_size + size
            ? pattern_byte(offset + index - prefix_size)
            : static_cast<uint8_t>(tail[index - prefix_size - size]);
    if (actual != expected) {
      Serial.print("RA4_STORAGE:MISMATCH_OFFSET=");
      Serial.println(index);
      response[0] = actual;
      response_size = 1;
      fail(stage);
    }
  }
}

}  // namespace

void setup()
{
  Serial.begin(kBaud);
  Serial2.begin(kBaud);
  delay(5000);
  Serial.println("RA4_STORAGE:START");

  static const uint8_t mount[] =
      "+MOUNTFS: FS Mounted\r\n\r\nOK\r\n";
  require("mount", "AT+MOUNTFS=1\r\n", nullptr, 0, mount, sizeof(mount) - 1);

  static const uint8_t write_ok[] = "OK\r\n";
  static const uint8_t file_data[] = {'a', 0, 'b', '\r', '\n'};
  require(
      "file-write",
      "AT+FS=0,1,/codex-storage.bin,5\r\n",
      file_data,
      sizeof(file_data),
      write_ok,
      sizeof(write_ok) - 1);

  static const uint8_t append_data[] = {'+', 'x'};
  require(
      "file-append",
      "AT+FS=0,3,/codex-storage.bin,2\r\n",
      append_data,
      sizeof(append_data),
      write_ok,
      sizeof(write_ok) - 1);

  static const uint8_t file_read[] =
      "+FS: 7|a\0b\r\n+x\r\nOK\r\n";
  require(
      "file-read",
      "AT+FS=0,2,/codex-storage.bin,0,32\r\n",
      nullptr,
      0,
      file_read,
      sizeof(file_read) - 1);
  require(
      "file-delete",
      "AT+FS=0,0,/codex-storage.bin\r\n",
      nullptr,
      0,
      write_ok,
      sizeof(write_ok) - 1);

  write_pattern("file-large-write",
      "AT+FS=0,1,/codex-storage-large.bin,16384\r\n", 16384, "OK\r\n");
  read_pattern("file-large-read",
      "AT+FS=0,2,/codex-storage-large.bin,0,16384\r\n",
      "+FS: 16384|", 0, 16384);
  read_pattern("file-large-offset-read",
      "AT+FS=0,2,/codex-storage-large.bin,255,513\r\n",
      "+FS: 513|", 255, 513);
  require("file-large-delete", "AT+FS=0,0,/codex-storage-large.bin\r\n",
      nullptr, 0, write_ok, sizeof(write_ok) - 1);
  Serial.println("RA4_STORAGE:LARGE_FILE_PASS=16384");

  static const uint8_t pref_begin[] =
      "+PREFBEGIN: 1\r\n\r\nOK\r\n";
  require(
      "pref-begin",
      "AT+PREFBEGIN=codex,0,\r\n",
      nullptr,
      0,
      pref_begin,
      sizeof(pref_begin) - 1);

  static const uint8_t pref_clear[] = "+PREFCLEAR: 1\r\nOK\r\n";
  require(
      "pref-clear",
      "AT+PREFCLEAR\r\n",
      nullptr,
      0,
      pref_clear,
      sizeof(pref_clear) - 1);

  static const uint8_t pref_put_i32[] = "+PREFPUT: 4\r\nOK\r\n";
  require(
      "pref-put-i32",
      "AT+PREFPUT=count,4,-123456\r\n",
      nullptr,
      0,
      pref_put_i32,
      sizeof(pref_put_i32) - 1);
  static const uint8_t pref_get_i32[] = "+PREFGET: -123456\r\nOK\r\n";
  require(
      "pref-get-i32",
      "AT+PREFGET=count,4,0\r\n",
      nullptr,
      0,
      pref_get_i32,
      sizeof(pref_get_i32) - 1);

  static const uint8_t pref_text[] = {'h', 'e', 'l', 'l', 'o'};
  static const uint8_t pref_put_text[] = "+PREFPUT: 5\r\nOK\r\n";
  require(
      "pref-put-string",
      "AT+PREFPUT=text,8,5\r\n",
      pref_text,
      sizeof(pref_text),
      pref_put_text,
      sizeof(pref_put_text) - 1);
  static const uint8_t pref_get_text[] =
      "+PREFGET: 5|hello\r\nOK\r\n";
  require(
      "pref-get-string",
      "AT+PREFGET=text,8\r\n",
      nullptr,
      0,
      pref_get_text,
      sizeof(pref_get_text) - 1);

  static const uint8_t pref_blob[] = {0x00, 0x7f, 0x80, 0xff};
  static const uint8_t pref_put_blob[] = "+PREFPUT: 4\r\nOK\r\n";
  require(
      "pref-put-blob",
      "AT+PREFPUT=blob,9,4\r\n",
      pref_blob,
      sizeof(pref_blob),
      pref_put_blob,
      sizeof(pref_put_blob) - 1);
  static const uint8_t pref_get_blob[] =
      {'+', 'P', 'R', 'E', 'F', 'G', 'E', 'T', ':', ' ', '4', '|',
       0x00, 0x7f, 0x80, 0xff, '\r', '\n', 'O', 'K', '\r', '\n'};
  require(
      "pref-get-blob",
      "AT+PREFGET=blob,9\r\n",
      nullptr,
      0,
      pref_get_blob,
      sizeof(pref_get_blob));

  static const uint8_t pref_type[] = "+PREFTYPE: 4\r\nOK\r\n";
  require(
      "pref-type",
      "AT+PREFTYPE=count\r\n",
      nullptr,
      0,
      pref_type,
      sizeof(pref_type) - 1);
  static const uint8_t pref_len[] = "+PREFLEN: 4\r\nOK\r\n";
  require(
      "pref-len",
      "AT+PREFLEN=blob\r\n",
      nullptr,
      0,
      pref_len,
      sizeof(pref_len) - 1);

  write_pattern("pref-large-put", "AT+PREFPUT=large,9,1024\r\n", 1024,
      "+PREFPUT: 1024\r\nOK\r\n");
  read_pattern("pref-large-get", "AT+PREFGET=large,9\r\n",
      "+PREFGET: 1024|", 0, 1024);
  Serial.println("RA4_STORAGE:LARGE_BLOB_PASS=1024");

  static const uint8_t pref_remove[] = "+PREFREMOVE: 1\r\nOK\r\n";
  require("pref-large-remove", "AT+PREFREMOVE=large\r\n",
      nullptr, 0, pref_remove, sizeof(pref_remove) - 1);
  require(
      "pref-remove",
      "AT+PREFREMOVE=count\r\n",
      nullptr,
      0,
      pref_remove,
      sizeof(pref_remove) - 1);
  static const uint8_t pref_end[] =
      "+PREFEND: 0\r\n\r\nOK\r\n";
  require(
      "pref-end",
      "AT+PREFEND\r\n",
      nullptr,
      0,
      pref_end,
      sizeof(pref_end) - 1);

  Serial.println("RA4_STORAGE:RESULT=PASS");
}

void loop()
{
  delay(1000);
}
