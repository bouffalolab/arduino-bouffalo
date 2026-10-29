/*
 * Temporary ESP32-S3 AT UART proxy for Arduino UNO R4 WiFi.
 *
 * The normal USB CDC port reaches the RA4M1 user UART, not the ESP32-S3 AT
 * UART. This sketch forwards PC USB CDC (Serial) to the ESP32-S3 AT UART
 * (Serial2) so host tooling can capture byte-accurate AT transcripts.
 *
 * Uploading this sketch replaces the current RA4M1 user sketch. It deliberately
 * emits no text and does not reproduce the bridge's 1200/2400 baud control
 * behavior. Keep both sides at kAtBaud while using it.
 */

namespace {

constexpr unsigned long kAtBaud = 115200;
constexpr size_t kMaxBytesPerDirectionPerLoop = 64;

void forwardAvailable(Stream& source, Stream& destination) {
  for (size_t count = 0;
       count < kMaxBytesPerDirectionPerLoop && source.available() > 0;
       ++count) {
    const int value = source.peek();
    if (value < 0) {
      return;
    }

    if (destination.write(static_cast<uint8_t>(value)) != 1) {
      return;
    }

    source.read();
  }
}

}  // namespace

void setup() {
  Serial.begin(kAtBaud);
  Serial2.begin(kAtBaud);
}

void loop() {
  forwardAvailable(Serial, Serial2);
  forwardAvailable(Serial2, Serial);
}
