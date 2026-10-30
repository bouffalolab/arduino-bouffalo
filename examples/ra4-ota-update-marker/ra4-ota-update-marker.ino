namespace {

constexpr unsigned long kBaud = 115200UL;
constexpr unsigned long kIntervalMs = 1000UL;
unsigned long next_report_ms = 0UL;
unsigned long report_count = 0UL;

void report()
{
  Serial.print("RA4_OTA_UPDATE_MARKER:");
  Serial.println(report_count++);
}

}  // namespace

void setup()
{
  Serial.begin(kBaud);
  delay(100);
  Serial.println("RA4_OTA_UPDATE_MARKER:BOOT");
  next_report_ms = millis();
}

void loop()
{
  const unsigned long now = millis();
  if (static_cast<long>(now - next_report_ms) >= 0) {
    report();
    next_report_ms = now + kIntervalMs;
  }
}
