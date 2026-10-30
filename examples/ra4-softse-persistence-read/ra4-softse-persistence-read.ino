#include <SoftwareATSE.h>

#include <string.h>

namespace {

constexpr unsigned long kBaud = 115200UL;
constexpr int kKeySlot = 201;
constexpr int kDataSlot = 202;
constexpr uint8_t kSlotData[] = {
    'b', 'l', '6', '1', '6', 'c', 'l', 0x00, 0x80, 0xff,
};
constexpr uint8_t kDigest[] = {
    0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
    0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
    0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
    0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
};

void fail(const char *stage)
{
  Serial.print("RA4_SOFTSE_PERSIST_READ:FAIL:");
  Serial.println(stage);
  SATSE.end();
  delay(1000);
  NVIC_SystemReset();
}

}  // namespace

void setup()
{
  Serial.begin(kBaud);
  delay(5000);
  Serial.println("RA4_SOFTSE_PERSIST_READ:START");

  if (SATSE.begin("codexpersist", false) != 1) {
    fail("BEGIN");
  }
  uint8_t public_key[64] = {};
  uint8_t signature[64] = {};
  uint8_t loaded_slot[sizeof(kSlotData)] = {};
  if (SATSE.generatePublicKey(kKeySlot, public_key) != 1 ||
      SATSE.ecSign(kKeySlot, kDigest, signature) != 1 ||
      SATSE.ecdsaVerify(kDigest, signature, public_key) != 1 ||
      SATSE.readSlot(kDataSlot, loaded_slot, sizeof(loaded_slot)) != 1 ||
      memcmp(loaded_slot, kSlotData, sizeof(kSlotData)) != 0) {
    fail("READ");
  }
  SATSE.end();
  Serial.println("RA4_SOFTSE_PERSIST_READ:PASS");
}

void loop()
{
  delay(1000);
}
