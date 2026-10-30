#include <SoftwareATSE.h>

#include <string.h>

namespace {

constexpr unsigned long kBaud = 115200UL;
constexpr int kKeySlot = 101;
constexpr int kDataSlot = 102;
constexpr uint8_t kExpectedSha256[] = {
    0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
    0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
    0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
    0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
};
constexpr uint8_t kSlotData[] = {
    0x00, 0x10, 0x20, 0x7f, 0x80, 0xff, 's', 'e',
};

void fail(const char *stage)
{
  Serial.print("RA4_SOFTSE:FAIL:");
  Serial.println(stage);
  SATSE.end();
  delay(1000);
  NVIC_SystemReset();
}

bool all_zero(const uint8_t *data, size_t size)
{
  for (size_t index = 0U; index < size; ++index) {
    if (data[index] != 0U) {
      return false;
    }
  }
  return true;
}

}  // namespace

void setup()
{
  Serial.begin(kBaud);
  delay(5000);
  Serial.println("RA4_SOFTSE:START");

  if (SATSE.begin("codexse", false) != 1) {
    fail("BEGIN");
  }

  const String serial = SATSE.serialNumber();
  if (serial.length() != 16U) {
    fail("SERIAL");
  }

  uint8_t random_a[32] = {};
  uint8_t random_b[32] = {};
  if (SATSE.random(random_a, sizeof(random_a)) != 1 ||
      SATSE.random(random_b, sizeof(random_b)) != 1 ||
      all_zero(random_a, sizeof(random_a)) ||
      all_zero(random_b, sizeof(random_b)) ||
      memcmp(random_a, random_b, sizeof(random_a)) == 0) {
    fail("RANDOM");
  }

  uint8_t public_key[64] = {};
  uint8_t loaded_public_key[64] = {};
  if (SATSE.generatePrivateKey(kKeySlot, public_key) != 1 ||
      SATSE.generatePublicKey(kKeySlot, loaded_public_key) != 1 ||
      memcmp(public_key, loaded_public_key, sizeof(public_key)) != 0) {
    fail("KEY");
  }

  static constexpr uint8_t kMessage[] = {'a', 'b', 'c'};
  uint8_t digest[32] = {};
  if (SATSE.SHA256(kMessage, sizeof(kMessage), digest) != 1 ||
      memcmp(digest, kExpectedSha256, sizeof(digest)) != 0) {
    fail("SHA256");
  }

  uint8_t signature[64] = {};
  if (SATSE.ecSign(kKeySlot, digest, signature) != 1 ||
      SATSE.ecdsaVerify(digest, signature, public_key) != 1) {
    fail("SIGN_VERIFY");
  }
  signature[0] ^= 0x01U;
  if (SATSE.ecdsaVerify(digest, signature, public_key) != 0) {
    fail("VERIFY_TAMPER");
  }

  uint8_t loaded_slot[sizeof(kSlotData)] = {};
  if (SATSE.writeSlot(kDataSlot, kSlotData, sizeof(kSlotData)) != 1 ||
      SATSE.readSlot(kDataSlot, loaded_slot, sizeof(loaded_slot)) != 1 ||
      memcmp(loaded_slot, kSlotData, sizeof(kSlotData)) != 0) {
    fail("SLOT");
  }
  SATSE.end();

  if (SATSE.begin("codexse", true) != 1) {
    fail("READONLY_BEGIN");
  }
  uint8_t unexpected_key[64] = {};
  if (SATSE.generatePrivateKey(kKeySlot + 1, unexpected_key) != 0) {
    fail("READONLY_MUTATION");
  }
  if (SATSE.generatePublicKey(kKeySlot, loaded_public_key) != 1 ||
      memcmp(public_key, loaded_public_key, sizeof(public_key)) != 0) {
    fail("READONLY_READ");
  }
  SATSE.end();

  Serial.println("RA4_SOFTSE:RESULT=PASS");
}

void loop()
{
  delay(1000);
}
