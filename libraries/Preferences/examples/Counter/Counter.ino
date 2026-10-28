#include <Preferences.h>

Preferences preferences;

void setup() {
  Serial.begin(115200);
  delay(1500);
  // Provision LittleFS explicitly before using this example. Mount failures
  // never format existing data. Keep every filesystem call in one task.
  if (!preferences.begin("counter")) {
    Serial.print("Preferences begin failed: ");
    Serial.println(preferences.lastError());
    return;
  }
  uint32_t count = preferences.getUInt("boots", 0);
  if (preferences.lastError() && preferences.lastError() != LFS_ERR_NOENT) {
    Serial.println("Stored counter has an unexpected type or read error");
    preferences.end();
    return;
  }
  if (preferences.putUInt("boots", count + 1) != sizeof(count)) {
    Serial.print("Commit failed: ");
    Serial.println(preferences.lastError());
  } else {
    Serial.print("Committed boot count: ");
    Serial.println(count + 1);
  }
  preferences.end();
}

void loop() {}
