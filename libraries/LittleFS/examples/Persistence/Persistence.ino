#include <LittleFS.h>

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  if (!LittleFS.begin()) {
    Serial.print("Mount failed: ");
    Serial.println(LittleFS.lastError());
    Serial.println("Send FORMAT to explicitly initialize the reserved filesystem.");
    while (Serial.readStringUntil('\n') != "FORMAT") delay(10);
    if (!LittleFS.format() || !LittleFS.begin()) {
      Serial.println("Format or mount failed");
      return;
    }
  }
  uint32_t count = 0;
  File input = LittleFS.open("/boot-count");
  if (input) {
    if (input.read(reinterpret_cast<uint8_t *>(&count), sizeof(count)) != sizeof(count)) {
      Serial.println("Invalid counter file");
      return;
    }
    input.close();
  } else if (LittleFS.lastError() != LFS_ERR_NOENT) {
    Serial.println("Counter read failed");
    return;
  }
  ++count;
  File output = LittleFS.open("/boot-count.tmp", "w");
  if (!output || output.write(reinterpret_cast<const uint8_t *>(&count), sizeof(count)) != sizeof(count) ||
      !output.sync() || !output.close() || !LittleFS.rename("/boot-count.tmp", "/boot-count")) {
    Serial.println("Counter commit failed");
    return;
  }
  Serial.print("Committed boot count: ");
  Serial.println(count);
}
void loop() { delay(100); }
