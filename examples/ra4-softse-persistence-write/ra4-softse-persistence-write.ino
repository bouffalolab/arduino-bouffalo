#include <SoftwareATSE.h>

#include <string.h>

namespace {

constexpr unsigned long kBaud = 115200UL;
constexpr int kKeySlot = 201;
constexpr int kDataSlot = 202;
constexpr uint8_t kSlotData[] = {
    'b', 'l', '6', '1', '6', 'c', 'l', 0x00, 0x80, 0xff,
};

void fail(const char *stage)
{
  Serial.print("RA4_SOFTSE_PERSIST_WRITE:FAIL:");
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
  Serial.println("RA4_SOFTSE_PERSIST_WRITE:START");

  if (SATSE.begin("codexpersist", false) != 1) {
    fail("BEGIN");
  }
  uint8_t public_key[64] = {};
  if (SATSE.generatePrivateKey(kKeySlot, public_key) != 1 ||
      SATSE.writeSlot(kDataSlot, kSlotData, sizeof(kSlotData)) != 1) {
    fail("WRITE");
  }
  SATSE.end();
  Serial.println("RA4_SOFTSE_PERSIST_WRITE:PASS");
}

void loop()
{
  delay(1000);
}
