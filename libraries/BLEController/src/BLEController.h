#ifndef ARDUINO_BL_BLE_CONTROLLER_H
#define ARDUINO_BL_BLE_CONTROLLER_H

#include <Arduino.h>

/* One external Host, raw H4 bytes. Serialize begin/end/read/write in one app task.
 * begin initializes the Controller; the Host must then issue HCI Reset and setup.
 * end requires the Host to stop traffic first. No UART pins are claimed.
 */
class BLEControllerClass {
public:
    /**
     * @brief Initialize the upstream Controller and H4 worker resources; the external Host
     * must then issue HCI Reset.
     */
    bool begin();
    /**
     * @brief Stop Controller worker resources after the Host has stopped all traffic.
     */
    void end();
    /**
     * @brief Return the currently readable byte count without consuming input.
     */
    int available();
    /**
     * @brief Consume available raw H4 bytes without waiting; return the byte/count or -1 for
     * an empty single-byte read.
     *
     * @return A byte/count when available; the single-byte overload returns -1 when empty.
     */
    int read();
    /**
     * @brief Consume available raw H4 bytes without waiting; return the byte/count or -1 for
     * an empty single-byte read.
     *
     * @return A byte/count when available; the single-byte overload returns -1 when empty.
     */
    size_t read(uint8_t *data, size_t size);
    /**
     * @brief Queue raw H4 bytes for the Controller and return the accepted byte count.
     *
     * @param data Caller-owned H4 bytes to copy into the Controller input stream.
     * @param size Number of input bytes.
     * @return Number of bytes accepted; zero when the transport is inactive.
     */
    size_t write(const uint8_t *data, size_t size);
};

extern BLEControllerClass BLEController;

#endif
