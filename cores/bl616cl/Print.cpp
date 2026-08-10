#include "Arduino.h"
#include "Print.h"

#include <math.h>

size_t Print::write(const uint8_t *buffer, size_t size)
{
    size_t written = 0;
    while (written < size && write(buffer[written])) {
        ++written;
    }
    return written;
}

size_t Print::print(const __FlashStringHelper *value)
{
    const char *text = reinterpret_cast<const char *>(value);
    return write(text);
}

size_t Print::print(const String &value) { return write(value.c_str(), value.length()); }
size_t Print::print(const char value[]) { return write(value); }
size_t Print::print(char value) { return write(static_cast<uint8_t>(value)); }
size_t Print::print(unsigned char value, int base) { return print(static_cast<unsigned long>(value), base); }
size_t Print::print(int value, int base) { return print(static_cast<long>(value), base); }
size_t Print::print(unsigned int value, int base) { return print(static_cast<unsigned long>(value), base); }

size_t Print::print(long value, int base)
{
    if (base == 0) {
        return write(static_cast<uint8_t>(value));
    }
    if (base == 10 && value < 0) {
        return write('-') + printNumber(static_cast<unsigned long>(-value), 10);
    }
    return printNumber(static_cast<unsigned long>(value), static_cast<uint8_t>(base));
}

size_t Print::print(unsigned long value, int base)
{
    return base == 0 ? write(static_cast<uint8_t>(value)) : printNumber(value, static_cast<uint8_t>(base));
}

size_t Print::print(double value, int digits) { return printFloat(value, static_cast<uint8_t>(digits)); }
size_t Print::print(const Printable &value) { return value.printTo(*this); }

size_t Print::println() { return write("\r\n"); }

#define BL616CL_PRINTLN(type)                 \
    size_t Print::println(type value)          \
    {                                          \
        size_t written = print(value);          \
        return written + println();             \
    }

BL616CL_PRINTLN(const __FlashStringHelper *)
BL616CL_PRINTLN(const String &)
BL616CL_PRINTLN(const char *)
BL616CL_PRINTLN(char)
BL616CL_PRINTLN(const Printable &)

#undef BL616CL_PRINTLN

size_t Print::println(unsigned char value, int base) { return print(value, base) + println(); }
size_t Print::println(int value, int base) { return print(value, base) + println(); }
size_t Print::println(unsigned int value, int base) { return print(value, base) + println(); }
size_t Print::println(long value, int base) { return print(value, base) + println(); }
size_t Print::println(unsigned long value, int base) { return print(value, base) + println(); }
size_t Print::println(double value, int digits) { return print(value, digits) + println(); }

size_t Print::printNumber(unsigned long value, uint8_t base)
{
    char buffer[8 * sizeof(value) + 1];
    char *cursor = &buffer[sizeof(buffer) - 1];
    *cursor = '\0';
    if (base < 2) {
        base = 10;
    }
    do {
        unsigned long digit = value % base;
        value /= base;
        *--cursor = static_cast<char>(digit < 10 ? '0' + digit : 'A' + digit - 10);
    } while (value != 0);
    return write(cursor);
}

size_t Print::printFloat(double value, uint8_t digits)
{
    if (isnan(value)) {
        return print("nan");
    }
    if (isinf(value)) {
        return print("inf");
    }
    if (value > 4294967040.0 || value < -4294967040.0) {
        return print("ovf");
    }

    size_t written = 0;
    if (value < 0.0) {
        written += print('-');
        value = -value;
    }

    double rounding = 0.5;
    for (uint8_t index = 0; index < digits; ++index) {
        rounding /= 10.0;
    }
    value += rounding;

    unsigned long integer = static_cast<unsigned long>(value);
    double remainder = value - static_cast<double>(integer);
    written += print(integer);
    if (digits > 0) {
        written += print('.');
    }
    while (digits-- > 0) {
        remainder *= 10.0;
        unsigned int digit = static_cast<unsigned int>(remainder);
        written += print(digit);
        remainder -= digit;
    }
    return written;
}
