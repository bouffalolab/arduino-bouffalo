#pragma once

#include "api/HardwareSerial.h"

/*
 * USB CDC serial interface. Serial is the host-visible USB port; Serial1 and Serial2 are hardware
 * UARTs. The Core starts USB before setup(); begin/end do not change USB state, and the host
 * selects the baud rate. Writes are discarded when DTR is inactive; writes wait at most 100 ms
 * without progress when the host opens but does not read the port. Opening at 1200 bps and then
 * closing requests ROM download mode; setTouchReset(false) disables this behavior.
 */
class USBSerial : public arduino::HardwareSerial {
public:
    /**
     * @brief Accept Arduino serial initialization arguments; USB enumeration and host line
     * settings remain owned by the CDC port.
     */
    void begin(unsigned long baudrate) override;
    /**
     * @brief Accept Arduino serial initialization arguments; USB enumeration and host line
     * settings remain owned by the CDC port.
     */
    void begin(unsigned long baudrate, uint16_t config) override;
    /**
     * @brief Leave the Core-owned USB device active; this compatibility call does not tear
     * down CDC.
     */
    void end() override;
    /**
     * @brief Return the currently readable byte count without consuming input.
     */
    int available(void) override;
    /**
     * @brief Return the next input byte without consuming it, or -1 when none is available.
     */
    int peek(void) override;
    /**
     * @brief Consume available input bytes; return the byte/count or the interface-specific
     * empty/error value.
     */
    int read(void) override;
    /**
     * @brief Wait for pending CDC output with the port's bounded disconnect/stall policy.
     */
    void flush(void) override;
    /**
     * @brief Return the number of bytes that can currently be queued without waiting.
     */
    int availableForWrite(void) override;
    /**
     * @brief Write the supplied byte or buffer and return the accepted byte count.
     */
    size_t write(uint8_t c) override;
    /**
     * @brief Write the supplied byte or buffer and return the accepted byte count.
     */
    size_t write(const uint8_t *buffer, size_t size) override;
    using Print::write;
    /**
     * @brief True while the host has opened the port and asserted DTR.
     */
    operator bool() override;

    /**
     * @brief Host line parameters and control signals, named after the official SAMD USB
     * serial API.
     */
    uint32_t baud(void);
    /**
     * @brief Return the CDC stop-bit encoding selected by the host.
     */
    uint8_t stopbits(void);
    /**
     * @brief Return the CDC parity encoding selected by the host.
     */
    uint8_t paritytype(void);
    /**
     * @brief Return the host-selected number of data bits.
     */
    uint8_t numbits(void);
    /**
     * @brief Return the current host DTR state.
     */
    bool dtr(void);
    /**
     * @brief Return the current host RTS state.
     */
    bool rts(void);

    /**
     * @brief Enable reset into ROM on 1200 bps touch; enabled by default. Disable when the
     * application handles the line rate, as the bridge does.
     */
    void setTouchReset(bool enable);
    /**
     * @brief Call callback once after each SET_LINE_CODING or SET_CONTROL_LINE_STATE request;
     * NULL unregisters it. Run in the Core line task, above loop priority, with delay()
     * allowed. Read current settings using baud(), dtr() and rts(). Requests arriving during a
     * callback are delivered individually afterward with the latest settings; earlier requests
     * are not replayed. First registration creates the task and returns false on failure.
     *
     * @param callback Task-context callback, or nullptr to unregister.
     * @return true on success; false if the callback task cannot be created.
     */
    bool onLineChange(void (*callback)(void));
};

#if defined(USB_VID)
extern USBSerial Serial;
#endif
