#ifndef IPAddress_h
#define IPAddress_h

#include <stdint.h>

#include "Printable.h"
#include "WString.h"

class IPAddress : public Printable
{
public:
    IPAddress();
    IPAddress(uint8_t first, uint8_t second, uint8_t third, uint8_t fourth);
    explicit IPAddress(uint32_t address);
    explicit IPAddress(const uint8_t *address);

    bool fromString(const char *address);
    bool fromString(const String &address) { return fromString(address.c_str()); }

    operator uint32_t() const { return address_.dword; }
    uint8_t operator[](int index) const { return address_.bytes[index]; }
    uint8_t &operator[](int index) { return address_.bytes[index]; }

    bool operator==(const IPAddress &other) const { return address_.dword == other.address_.dword; }
    IPAddress &operator=(uint32_t address);
    IPAddress &operator=(const uint8_t *address);

    size_t printTo(Print &print) const override;
    String toString() const;

private:
    union {
        uint8_t bytes[4];
        uint32_t dword;
    } address_;
};

extern IPAddress INADDR_NONE;

#endif
