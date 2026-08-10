#include "stdlib_noniso.h"

#include <stdio.h>
#include <string.h>

static char *reverse(char *begin, char *end)
{
    while (begin < end) {
        char value = *begin;
        *begin++ = *end;
        *end-- = value;
    }
    return begin;
}

char *ultoa(unsigned long value, char *buffer, int base)
{
    if (base < 2 || base > 36) {
        buffer[0] = '\0';
        return buffer;
    }
    char *cursor = buffer;
    do {
        unsigned long digit = value % (unsigned long)base;
        *cursor++ = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
        value /= (unsigned long)base;
    } while (value != 0);
    *cursor = '\0';
    reverse(buffer, cursor - 1);
    return buffer;
}

char *ltoa(long value, char *buffer, int base)
{
    if (value < 0 && base == 10) {
        buffer[0] = '-';
        ultoa((unsigned long)(-value), buffer + 1, base);
        return buffer;
    }
    return ultoa((unsigned long)value, buffer, base);
}

char *utoa(unsigned int value, char *buffer, int base)
{
    return ultoa(value, buffer, base);
}

char *dtostrf(double value, signed char width, unsigned char precision, char *buffer)
{
    char format[16];
    snprintf(format, sizeof(format), "%%%d.%df", (int)width, (int)precision);
    snprintf(buffer, 64, format, value);
    return buffer;
}
