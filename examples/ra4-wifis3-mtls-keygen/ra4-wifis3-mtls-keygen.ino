#include <SoftwareATSE.h>

namespace {

constexpr unsigned long kBaud = 115200UL;
constexpr unsigned long kRepeatIntervalMs = 1000UL;
constexpr int kKeySlot = 301;
constexpr char kNamespace[] = "codexmtls";

uint8_t public_key[64] = {};
bool completed = false;
unsigned long next_announcement_ms = 0UL;

void print_hex(const uint8_t *data, size_t size)
{
  static constexpr char kHex[] = "0123456789ABCDEF";
  for (size_t index = 0U; index < size; ++index) {
    Serial.print(kHex[data[index] >> 4U]);
    Serial.print(kHex[data[index] & 0x0FU]);
  }
}

void fail(const char *stage)
{
  Serial.print("RA4_MTLS_KEYGEN:FAIL:");
  Serial.println(stage);
  SATSE.end();
  completed = true;
}

void announce_result()
{
  Serial.print("RA4_MTLS_KEYGEN:PUBKEY=");
  print_hex(public_key, sizeof(public_key));
  Serial.println();
  Serial.println("RA4_MTLS_KEYGEN:RESULT=PASS");
}

}  // namespace

void setup()
{
  Serial.begin(kBaud);
  delay(1500);
  Serial.println("RA4_MTLS_KEYGEN:START");

  if (SATSE.begin(kNamespace, false) != 1) {
    fail("BEGIN");
    return;
  }

  if (SATSE.generatePrivateKey(kKeySlot, public_key) != 1) {
    fail("KEY");
    return;
  }
  SATSE.end();

  completed = true;
  announce_result();
  next_announcement_ms = millis() + kRepeatIntervalMs;
}

void loop()
{
  if (completed &&
      static_cast<long>(millis() - next_announcement_ms) >= 0) {
    announce_result();
    next_announcement_ms += kRepeatIntervalMs;
  }
  delay(10);
}
