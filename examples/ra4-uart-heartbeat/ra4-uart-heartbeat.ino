/*
 * Temporary paired-board smoke Sketch.
 *
 * The UNO R4 WiFi build defines NO_USB, so Serial is RA4M1 UART1 on
 * P109/P110, bridged by BL616CL GPIO34/GPIO35. Serial1 is instead the
 * D1/D0 UART, and Serial2 is the Wi-Fi AT UART. Keep this Sketch on Serial
 * so it validates the split user-UART path.
 */

namespace {

#ifndef RA4_USER_UART_BAUD
#define RA4_USER_UART_BAUD 115200UL
#endif

constexpr unsigned long kUserUartBaud = RA4_USER_UART_BAUD;
constexpr unsigned long kHeartbeatIntervalMs = 1000;
char line_buffer[96];
size_t line_length = 0;
unsigned long next_heartbeat_ms = 0;
unsigned long heartbeat_count = 0;

void emit_heartbeat()
{
  Serial.print("RA4_HEARTBEAT:");
  Serial.println(heartbeat_count++);
}

void emit_echo()
{
  Serial.print("RA4_ECHO:");
  Serial.println(line_buffer);
  line_length = 0;
}

} // namespace

void setup()
{
  Serial.begin(kUserUartBaud);
  delay(100);
  Serial.println("RA4_HEARTBEAT:BOOT");
  next_heartbeat_ms = millis();
}

void loop()
{
  const unsigned long now = millis();
  if (static_cast<long>(now - next_heartbeat_ms) >= 0) {
    emit_heartbeat();
    next_heartbeat_ms = now + kHeartbeatIntervalMs;
  }

  while (Serial.available() > 0) {
    const char value = static_cast<char>(Serial.read());

    if (value == '\r') {
      continue;
    }
    if (value == '\n') {
      emit_echo();
      continue;
    }
    if (line_length + 1 < sizeof(line_buffer)) {
      line_buffer[line_length++] = value;
      line_buffer[line_length] = '\0';
    } else {
      line_length = 0;
      Serial.println("RA4_ECHO:OVERFLOW");
    }
  }
}
