#include "Arduino.h"
#include "IPAddress.h"

#include <stdio.h>
#include <string.h>

IPAddress::IPAddress() { address_.dword = 0; }
IPAddress::IPAddress(uint8_t first, uint8_t second, uint8_t third, uint8_t fourth)
{
    address_.bytes[0] = first;
    address_.bytes[1] = second;
    address_.bytes[2] = third;
    address_.bytes[3] = fourth;
}
IPAddress::IPAddress(uint32_t address) { address_.dword = address; }
IPAddress::IPAddress(const uint8_t *address) { memcpy(address_.bytes, address, sizeof(address_.bytes)); }

IPAddress &IPAddress::operator=(uint32_t address)
{
    address_.dword = address;
    return *this;
}

IPAddress &IPAddress::operator=(const uint8_t *address)
{
    memcpy(address_.bytes, address, sizeof(address_.bytes));
    return *this;
}

bool IPAddress::fromString(const char *text)
{
    if (text == nullptr) {
        return false;
    }
    uint8_t parsed[4] = {};
    unsigned int octet = 0;
    unsigned int value = 0;
    bool have_digit = false;
    for (;;) {
        char character = *text++;
        if (character >= '0' && character <= '9') {
            have_digit = true;
            value = value * 10U + static_cast<unsigned int>(character - '0');
            if (value > 255U) {
                return false;
            }
        } else if (character == '.' || character == '\0') {
            if (!have_digit || octet >= 4U) {
                return false;
            }
            parsed[octet++] = static_cast<uint8_t>(value);
            value = 0;
            have_digit = false;
            if (character == '\0') {
                break;
            }
        } else {
            return false;
        }
    }
    if (octet != 4U) {
        return false;
    }
    memcpy(address_.bytes, parsed, sizeof(parsed));
    return true;
}

size_t IPAddress::printTo(Print &print) const
{
    size_t written = 0;
    for (int index = 0; index < 4; ++index) {
        written += print.print(address_.bytes[index]);
        if (index != 3) {
            written += print.print('.');
        }
    }
    return written;
}

String IPAddress::toString() const
{
    char text[16];
    snprintf(text, sizeof(text), "%u.%u.%u.%u", address_.bytes[0], address_.bytes[1],
             address_.bytes[2], address_.bytes[3]);
    return String(text);
}

IPAddress INADDR_NONE(0, 0, 0, 0);
