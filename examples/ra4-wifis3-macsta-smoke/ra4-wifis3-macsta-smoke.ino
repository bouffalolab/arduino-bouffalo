#include <WiFiS3.h>

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("RA4_WIFI_S3_MACSTA_READY");
}

void loop() {
  uint8_t mac[WL_MAC_ADDR_LENGTH] = {};
  WiFi.macAddress(mac);
  bool nonzero = false;
  bool all_ff = true;
  for (size_t index = 0; index < WL_MAC_ADDR_LENGTH; ++index) {
    if (mac[index] != 0U) {
      nonzero = true;
    }
    if (mac[index] != 0xffU) all_ff = false;
  }
  Serial.println(nonzero && !all_ff && (mac[0] & 1U) == 0U
                     ? "RA4_WIFI_S3_MACSTA_PASS"
                     : "RA4_WIFI_S3_MACSTA_FAIL");
  delay(3000);
}
