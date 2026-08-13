#ifndef HardwareSerial_h
#define HardwareSerial_h

#include <inttypes.h>

#include "Stream.h"

#define SERIAL_5N1 0x10
#define SERIAL_6N1 0x11
#define SERIAL_7N1 0x12
#define SERIAL_8N1 0x13
#define SERIAL_5N2 0x30
#define SERIAL_6N2 0x31
#define SERIAL_7N2 0x32
#define SERIAL_8N2 0x33
#define SERIAL_5E1 0x90
#define SERIAL_6E1 0x91
#define SERIAL_7E1 0x92
#define SERIAL_8E1 0x93
#define SERIAL_5E2 0xB0
#define SERIAL_6E2 0xB1
#define SERIAL_7E2 0xB2
#define SERIAL_8E2 0xB3
#define SERIAL_5O1 0x50
#define SERIAL_6O1 0x51
#define SERIAL_7O1 0x52
#define SERIAL_8O1 0x53
#define SERIAL_5O2 0x70
#define SERIAL_6O2 0x71
#define SERIAL_7O2 0x72
#define SERIAL_8O2 0x73

struct bflb_device_s;

class HardwareSerial : public Stream
{
public:
    explicit HardwareSerial(uint8_t index, int8_t rx_pin, int8_t tx_pin);

    void begin(unsigned long baud, uint8_t config = SERIAL_8N1);
    void begin(unsigned long baud, uint8_t config, int8_t rxPin, int8_t txPin);
    void end();
    uint32_t baudRate() const { return baud_rate_; }
    void updateBaudRate(uint32_t baud);
    void setRxBufferSize(size_t size) { (void)size; }
    void setTxBufferSize(size_t size) { (void)size; }

    int available() override;
    int peek() override;
    int read() override;
    size_t read(uint8_t *buffer, size_t size);
    int availableForWrite() override;
    void flush() override;
    size_t write(uint8_t value) override;
    size_t write(const uint8_t *buffer, size_t size) override;
    using Print::write;

    operator bool() const { return device_ != nullptr; }

private:
    uint8_t index_;
    int8_t rx_pin_;
    int8_t tx_pin_;
    int peeked_;
    uint32_t baud_rate_;
    struct bflb_device_s *device_;
};

extern HardwareSerial Serial;
extern HardwareSerial Serial1;

void serialEventRun(void);

#endif
