#include <WiFiS3.h>

constexpr unsigned long kScanIntervalMs = 10000;

const char *encryptionName(uint8_t encryptionType) {
  switch (encryptionType) {
    case ENC_TYPE_NONE:
      return "open";
    case ENC_TYPE_WEP:
      return "WEP";
    case ENC_TYPE_TKIP:
      return "WPA";
    case ENC_TYPE_CCMP:
      return "WPA2";
    case ENC_TYPE_AUTO:
      return "auto";
    default:
      return "unknown";
  }
}

void printScanResults() {
  const int networkCount = WiFi.scanNetworks();

  if (networkCount == WL_NO_MODULE) {
    Serial.println("ERROR: WiFi module not detected.");
    return;
  }

  if (networkCount < 0) {
    Serial.print("ERROR: WiFi scan failed, code=");
    Serial.println(networkCount);
    return;
  }

  Serial.print("Networks found: ");
  Serial.println(networkCount);

  for (int index = 0; index < networkCount; ++index) {
    Serial.print(index + 1);
    Serial.print(". SSID: ");
    Serial.print(WiFi.SSID(index));
    Serial.print(" | RSSI: ");
    Serial.print(WiFi.RSSI(index));
    Serial.print(" dBm | Security: ");
    Serial.println(encryptionName(WiFi.encryptionType(index)));
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("UNO R4 WiFi bridge smoke test");
  Serial.print("WiFi firmware: ");
  Serial.println(WiFi.firmwareVersion());
}

void loop() {
  printScanResults();
  Serial.println("---");
  delay(kScanIntervalMs);
}
