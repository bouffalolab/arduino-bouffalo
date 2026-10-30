/*
 * Temporary RA4M1 SRAM target for CMSIS-DAP D6 validation.
 *
 * Keep this buffer isolated from the runtime stack, heap, vector table, and
 * application logic. The address is printed so host-side tools can verify
 * the exact linker placement before issuing SWD writes.
 */

namespace {

constexpr size_t kScratchWords = 16;
constexpr uint32_t kScratchSeed = 0xA5A50000UL;

volatile uint32_t swd_scratch[kScratchWords]
    __attribute__((used, aligned(4)));

void initialize_scratch()
{
  for (size_t index = 0; index < kScratchWords; ++index) {
    swd_scratch[index] = kScratchSeed + static_cast<uint32_t>(index);
  }
}

}  // namespace

void setup()
{
  Serial.begin(115200);
  initialize_scratch();
}

void loop()
{
  Serial.print("RA4_SWD_SCRATCH:ADDR=0x");
  Serial.println(
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(swd_scratch)),
      HEX);
  Serial.print("RA4_SWD_SCRATCH:BYTES=");
  Serial.println(sizeof(swd_scratch));
  Serial.println("RA4_SWD_SCRATCH:READY");
  Serial.println("RA4_SWD_SCRATCH:RUNNING");
  delay(1000);
}
