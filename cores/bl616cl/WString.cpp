#include "WString.h"

#include "stdlib_noniso.h"

#include <stdio.h>

void String::init()
{
    buffer_ = nullptr;
    capacity_ = 0;
    length_ = 0;
}

String::String(const char *value)
{
    init();
    assign(value == nullptr ? "" : value, value == nullptr ? 0 : strlen(value));
}

String::String(const String &value)
{
    init();
    assign(value.c_str(), value.length_);
}

String::String(String &&value) noexcept
    : buffer_(value.buffer_), capacity_(value.capacity_), length_(value.length_)
{
    value.init();
}

String::String(char value)
{
    init();
    char text[2] = {value, '\0'};
    assign(text, 1);
}

String::String(unsigned char value, unsigned char base)
{
    init();
    char text[1 + 8 * sizeof(value)];
    utoa(value, text, base);
    assign(text, strlen(text));
}

String::String(int value, unsigned char base)
{
    init();
    char text[2 + 8 * sizeof(value)];
    itoa(value, text, base);
    assign(text, strlen(text));
}

String::String(unsigned int value, unsigned char base)
{
    init();
    char text[1 + 8 * sizeof(value)];
    utoa(value, text, base);
    assign(text, strlen(text));
}

String::String(long value, unsigned char base)
{
    init();
    char text[2 + 8 * sizeof(value)];
    ltoa(value, text, base);
    assign(text, strlen(text));
}

String::String(unsigned long value, unsigned char base)
{
    init();
    char text[1 + 8 * sizeof(value)];
    ultoa(value, text, base);
    assign(text, strlen(text));
}

String::String(long long value, unsigned char base)
{
    init();
    char text[2 + 8 * sizeof(value)];
    snprintf(text, sizeof(text), "%lld", static_cast<long long>(value));
    assign(text, strlen(text));
}

String::String(unsigned long long value, unsigned char base)
{
    init();
    char text[1 + 8 * sizeof(value)];
    snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(value));
    assign(text, strlen(text));
}

String::String(float value, unsigned char decimal_places)
{
    init();
    char text[40];
    dtostrf(value, decimal_places + 2, decimal_places, text);
    assign(text, strlen(text));
}

String::String(double value, unsigned char decimal_places)
{
    init();
    char text[40];
    dtostrf(value, decimal_places + 2, decimal_places, text);
    assign(text, strlen(text));
}

String::~String()
{
    free(buffer_);
}

String &String::operator=(const String &value)
{
    if (this != &value) {
        assign(value.c_str(), value.length_);
    }
    return *this;
}

String &String::operator=(String &&value) noexcept
{
    if (this != &value) {
        free(buffer_);
        buffer_ = value.buffer_;
        capacity_ = value.capacity_;
        length_ = value.length_;
        value.init();
    }
    return *this;
}

String &String::operator=(const char *value)
{
    assign(value == nullptr ? "" : value, value == nullptr ? 0 : strlen(value));
    return *this;
}

bool String::reserve(unsigned int size)
{
    if (capacity_ >= size && buffer_ != nullptr) {
        return true;
    }
    char *new_buffer = static_cast<char *>(realloc(buffer_, size + 1));
    if (new_buffer == nullptr) {
        return false;
    }
    buffer_ = new_buffer;
    capacity_ = size;
    if (length_ == 0) {
        buffer_[0] = '\0';
    }
    return true;
}

bool String::assign(const char *value, unsigned int length)
{
    if (!reserve(length)) {
        return false;
    }
    memcpy(buffer_, value, length);
    buffer_[length] = '\0';
    length_ = length;
    return true;
}

bool String::concat(const char *value, unsigned int length)
{
    if (value == nullptr || length == 0) {
        return value != nullptr;
    }
    unsigned int new_length = length_ + length;
    if (!reserve(new_length)) {
        return false;
    }
    memcpy(buffer_ + length_, value, length);
    length_ = new_length;
    buffer_[length_] = '\0';
    return true;
}

bool String::concat(const String &value) { return concat(value.c_str(), value.length_); }
bool String::concat(const char *value) { return concat(value, value == nullptr ? 0 : strlen(value)); }
bool String::concat(char value) { return concat(&value, 1); }

#define STRING_CONCAT_NUMBER(type)        \
    bool String::concat(type value)       \
    {                                     \
        return concat(String(value));      \
    }

STRING_CONCAT_NUMBER(unsigned char)
STRING_CONCAT_NUMBER(int)
STRING_CONCAT_NUMBER(unsigned int)
STRING_CONCAT_NUMBER(long)
STRING_CONCAT_NUMBER(unsigned long)
STRING_CONCAT_NUMBER(float)
STRING_CONCAT_NUMBER(double)

#undef STRING_CONCAT_NUMBER

char String::operator[](unsigned int index) const
{
    return index < length_ ? buffer_[index] : '\0';
}

char &String::operator[](unsigned int index)
{
    static char invalid = '\0';
    if (index >= length_) {
        invalid = '\0';
        return invalid;
    }
    return buffer_[index];
}

String operator+(const String &left, const String &right)
{
    String result(left);
    result += right;
    return result;
}

String operator+(const String &left, const char *right)
{
    String result(left);
    result += right;
    return result;
}

String operator+(const char *left, const String &right)
{
    String result(left);
    result += right;
    return result;
}
