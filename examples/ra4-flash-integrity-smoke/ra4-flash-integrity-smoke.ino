#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

// A full-image checksum from the RA4 CPU avoids relying on SAM-BA's optional
// Z command. This fixture is for upload validation, not a bootloader change.
// Retain a nonzero 96 KiB pattern to exercise more than the first flash page.
#define P32 0x71,0x03,0x9a,0xe5,0x12,0xc4,0x68,0xb7,0xff,0x00,0x35,0x82,0xda,0x4e,0x16,0xab,0x28,0x94,0x60,0xd3,0x07,0xf1,0x5c,0x8b,0x39,0xae,0x42,0x1d,0xc0,0x76,0xeb,0x55
#define P64 P32,P32
#define P128 P64,P64
#define P256 P128,P128
#define P512 P256,P256
#define P1024 P512,P512
#define P2048 P1024,P1024
#define P4096 P2048,P2048
#define P8192 P4096,P4096
#define P16384 P8192,P8192
#define P32768 P16384,P16384
const uint8_t flash_pattern[] = {P32768,P32768,P32768};

namespace {
char command[32];
unsigned used;
unsigned long announce;
uint32_t crc32(const volatile uint8_t *data, size_t length) {
  uint32_t value = 0xffffffffU;
  for (size_t i = 0; i < length; ++i) {
    value ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      value = (value >> 1) ^ ((value & 1) ? 0xedb88320U : 0U);
  }
  return ~value;
}
void check() {
  char *end = nullptr;
  const unsigned long length = strtoul(command + 6, &end, 10);
  if (strncmp(command, "CHECK ", 6) || end == command + 6 || *end ||
      !length || length > 0x3c000UL) {
    Serial.println("FLASH_INTEGRITY:FAIL=COMMAND");
    return;
  }
  // renesas_uno UNO R4 WiFi application .text LMA is 0x4000; BOSSA's
  // logical application offset zero maps here (not the bootloader at zero).
  const uint32_t value = crc32(reinterpret_cast<const volatile uint8_t *>(0x4000), length);
  Serial.print("FLASH_INTEGRITY:SIZE="); Serial.print(length);
  Serial.print(" CRC32="); Serial.print(value, HEX);
  Serial.print(" PROBE="); Serial.println(flash_pattern[length % sizeof(flash_pattern)], HEX);
}
}
void setup() { Serial.begin(115200); }
void loop() {
  while (Serial.available()) {
    const int c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { command[used] = 0; check(); used = 0; }
    else if (used + 1 < sizeof(command)) command[used++] = static_cast<char>(c);
    else { used = 0; Serial.println("FLASH_INTEGRITY:FAIL=OVERFLOW"); }
  }
  if (static_cast<long>(millis() - announce) >= 0) {
    Serial.println("FLASH_INTEGRITY:READY"); announce = millis() + 1000;
  }
}
