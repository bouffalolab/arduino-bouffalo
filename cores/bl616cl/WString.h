#ifndef String_class_h
#define String_class_h

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "pgmspace.h"

class __FlashStringHelper;
#define F(value) (reinterpret_cast<const __FlashStringHelper *>(PSTR(value)))

class String
{
public:
    String(const char *value = "");
    String(const String &value);
    String(String &&value) noexcept;
    String(char value);
    String(unsigned char value, unsigned char base = 10);
    String(int value, unsigned char base = 10);
    String(unsigned int value, unsigned char base = 10);
    String(long value, unsigned char base = 10);
    String(unsigned long value, unsigned char base = 10);
    String(long long value, unsigned char base = 10);
    String(unsigned long long value, unsigned char base = 10);
    String(float value, unsigned char decimal_places = 2);
    String(double value, unsigned char decimal_places = 2);
    ~String();

    String &operator=(const String &value);
    String &operator=(String &&value) noexcept;
    String &operator=(const char *value);

    bool reserve(unsigned int size);
    unsigned int length() const { return length_; }
    const char *c_str() const { return buffer_ == nullptr ? "" : buffer_; }

    bool concat(const String &value);
    bool concat(const char *value);
    bool concat(char value);
    bool concat(unsigned char value);
    bool concat(int value);
    bool concat(unsigned int value);
    bool concat(long value);
    bool concat(unsigned long value);
    bool concat(float value);
    bool concat(double value);

    String &operator+=(const String &value) { concat(value); return *this; }
    String &operator+=(const char *value) { concat(value); return *this; }
    String &operator+=(char value) { concat(value); return *this; }
    String &operator+=(unsigned char value) { concat(value); return *this; }
    String &operator+=(int value) { concat(value); return *this; }
    String &operator+=(unsigned int value) { concat(value); return *this; }
    String &operator+=(long value) { concat(value); return *this; }
    String &operator+=(unsigned long value) { concat(value); return *this; }
    String &operator+=(float value) { concat(value); return *this; }
    String &operator+=(double value) { concat(value); return *this; }

    bool operator==(const String &value) const { return strcmp(c_str(), value.c_str()) == 0; }
    bool operator!=(const String &value) const { return !(*this == value); }
    bool operator==(const char *value) const { return strcmp(c_str(), value == nullptr ? "" : value) == 0; }
    bool operator!=(const char *value) const { return !(*this == value); }
    explicit operator bool() const { return buffer_ != nullptr; }
    operator std::string() const { return std::string(c_str()); }

    char operator[](unsigned int index) const;
    char &operator[](unsigned int index);

    long toInt() const { return atol(c_str()); }
    float toFloat() const { return static_cast<float>(atof(c_str())); }
    double toDouble() const { return atof(c_str()); }

private:
    bool concat(const char *value, unsigned int length);
    bool assign(const char *value, unsigned int length);
    void init();

    char *buffer_;
    unsigned int capacity_;
    unsigned int length_;
};

String operator+(const String &left, const String &right);
String operator+(const String &left, const char *right);
String operator+(const char *left, const String &right);

#endif
