/* Raw H4 external Host on USB CDC; BL616CL runs only the Controller.
 * Send HCI Reset (01 03 0c 00), then normal HCI setup from the external Host.
 * Requires bl616cl-libs 0.5.0. USB CDC still supports BL's 1200 bps upload touch.
 */
#include <BLEController.h>

static uint8_t pending[128];
static size_t count, offset;

/**
 * @brief Initialize the resources used by this example or validation sketch.
 */
void setup()
{
  Serial.begin(115200);
  if (!BLEController.begin()) {
    while (true) delay(1000);
  }
}

/**
 * @brief Service the example or validation scenario on each Arduino loop iteration.
 */
void loop()
{
  if (offset == count) {
    offset = 0;
    count = 0;
    while (count < sizeof(pending) && Serial.available()) {
      pending[count++] = Serial.read();
    }
  }
  if (offset < count) {
    offset += BLEController.write(pending + offset, count - offset);
  }
  uint8_t data[128];
  size_t size = BLEController.read(data, sizeof(data));
  if (size) Serial.write(data, size);
  delay(1);
}
