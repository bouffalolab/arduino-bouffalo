#include <WiFiS3.h>

namespace {

constexpr unsigned long kStartupAnnouncementCount = 5;
constexpr unsigned long kStartupAnnouncementIntervalMs = 1000;
constexpr unsigned long kResultRepeatIntervalMs = 1000;

const char *firmware_version = nullptr;
int network_count = 0;
unsigned long scan_elapsed_ms = 0;
const char *failure_reason = nullptr;
int failure_code = 0;

void print_failure() {
  Serial.print("RA4_WIFI_S3_SCAN:RESULT=FAIL:");
  Serial.print(failure_reason);
  Serial.print(":");
  Serial.println(failure_code);
}

void print_result() {
  if (failure_reason != nullptr) {
    print_failure();
    return;
  }

  Serial.print("RA4_WIFI_S3_SCAN:FIRMWARE=");
  Serial.println(firmware_version);
  Serial.print("RA4_WIFI_S3_SCAN:COUNT=");
  Serial.println(network_count);
  Serial.print("RA4_WIFI_S3_SCAN:ELAPSED_MS=");
  Serial.println(scan_elapsed_ms);
  Serial.println("RA4_WIFI_S3_SCAN:RESULT=PASS");
}

void report_failure(const char *reason, int code) {
  failure_reason = reason;
  failure_code = code;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  for (unsigned long index = 0; index < kStartupAnnouncementCount; ++index) {
    Serial.println("RA4_WIFI_S3_SCAN:BOOT");
    delay(kStartupAnnouncementIntervalMs);
  }

  firmware_version = WiFi.firmwareVersion();
  if (firmware_version == nullptr || firmware_version[0] == '\0') {
    report_failure("FIRMWARE_VERSION", -1);
    return;
  }

  const unsigned long started_at = millis();
  network_count = WiFi.scanNetworks();
  scan_elapsed_ms = millis() - started_at;

  if (network_count == WL_NO_MODULE) {
    report_failure("NO_MODULE", network_count);
  } else if (network_count < 0) {
    report_failure("SCAN", network_count);
  } else if (network_count == 0) {
    report_failure("EMPTY_SCAN", network_count);
  }
}

void loop() {
  print_result();
  delay(kResultRepeatIntervalMs);
}
