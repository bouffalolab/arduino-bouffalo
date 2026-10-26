#ifndef ARDUINO_BL_BLE_CONTROLLER_H
#define ARDUINO_BL_BLE_CONTROLLER_H

#include <Arduino.h>

/* One external Host, raw H4 bytes. Serialize begin/end/read/write in one app task.
 * begin initializes the Controller; the Host must then issue HCI Reset and setup.
 * end requires the Host to stop traffic first. No UART pins are claimed.
 */
class BLEControllerClass {
public:
    bool begin();
    void end();
    int available();
    int read();
    size_t read(uint8_t *data, size_t size);
    size_t write(const uint8_t *data, size_t size);
};

extern BLEControllerClass BLEController;

#endif
