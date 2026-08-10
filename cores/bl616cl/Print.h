#ifndef Print_h
#define Print_h

#include <inttypes.h>
#include <stddef.h>
#include <string.h>

#include "Printable.h"
#include "WString.h"

#define DEC 10
#define HEX 16
#define OCT 8
#ifdef BIN
#undef BIN
#endif
#define BIN 2

class Print
{
public:
    Print() : write_error_(0) {}
    virtual ~Print() = default;

    virtual size_t write(uint8_t value) = 0;
    virtual size_t write(const uint8_t *buffer, size_t size);
    size_t write(const char *text)
    {
        return text == nullptr ? 0 : write(reinterpret_cast<const uint8_t *>(text), strlen(text));
    }
    size_t write(const char *buffer, size_t size)
    {
        return write(reinterpret_cast<const uint8_t *>(buffer), size);
    }

    virtual int availableForWrite() { return 0; }
    virtual void flush() {}

    int getWriteError() const { return write_error_; }
    void clearWriteError() { write_error_ = 0; }

    size_t print(const __FlashStringHelper *value);
    size_t print(const String &value);
    size_t print(const char value[]);
    size_t print(char value);
    size_t print(unsigned char value, int base = DEC);
    size_t print(int value, int base = DEC);
    size_t print(unsigned int value, int base = DEC);
    size_t print(long value, int base = DEC);
    size_t print(unsigned long value, int base = DEC);
    size_t print(double value, int digits = 2);
    size_t print(const Printable &value);

    size_t println();
    size_t println(const __FlashStringHelper *value);
    size_t println(const String &value);
    size_t println(const char value[]);
    size_t println(char value);
    size_t println(unsigned char value, int base = DEC);
    size_t println(int value, int base = DEC);
    size_t println(unsigned int value, int base = DEC);
    size_t println(long value, int base = DEC);
    size_t println(unsigned long value, int base = DEC);
    size_t println(double value, int digits = 2);
    size_t println(const Printable &value);

protected:
    void setWriteError(int error = 1) { write_error_ = error; }

private:
    size_t printNumber(unsigned long value, uint8_t base);
    size_t printFloat(double value, uint8_t digits);
    int write_error_;
};

#endif
