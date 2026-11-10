#include <BLUpdate.h>

// A transport example. A sender waits for each prompt before sending raw bytes.
// Use an application-specific, authenticated transport for deployed devices.
uint8_t block[512];

void setup() {
#if defined(PIN_SERIAL_TX)
  Serial.setRxBufferSize(1024);
#endif
  Serial.begin(115200);
  Serial.setTimeout(10000);
}

void loop() {
  if (!Serial.available() || Serial.read() != 'U') return;

  Serial.println("HEADER 512");
  if (Serial.readBytes(block, 512) != 512 || !BLUpdate.begin(block, 512)) {
    BLUpdate.abort();
    Serial.println("ERROR header");
    return;
  }
  while (BLUpdate.written() < BLUpdate.size()) {
    size_t remaining = BLUpdate.size() - BLUpdate.written();
    size_t count = remaining < 256 ? remaining : 256;
    Serial.print("DATA ");
    Serial.println(count);
    if (Serial.readBytes(block, count) != count ||
        !BLUpdate.write(BLUpdate.written(), block, count)) {
      BLUpdate.abort();
      Serial.println("ERROR data");
      return;
    }
  }
  if (!BLUpdate.finish() || !BLUpdate.commit()) {
    BLUpdate.abort();
    Serial.println("ERROR verify or commit");
    return;
  }
  Serial.println("COMMITTED: reset board to start the new sketch");
}
