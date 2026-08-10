#include "Arduino.h"
#include "Stream.h"

int Stream::timedRead()
{
    start_millis_ = millis();
    do {
        int value = read();
        if (value >= 0) {
            return value;
        }
        yield();
    } while (millis() - start_millis_ < timeout_);
    return -1;
}

int Stream::timedPeek()
{
    start_millis_ = millis();
    do {
        int value = peek();
        if (value >= 0) {
            return value;
        }
        yield();
    } while (millis() - start_millis_ < timeout_);
    return -1;
}

size_t Stream::readBytes(char *buffer, size_t length)
{
    size_t count = 0;
    while (count < length) {
        int value = timedRead();
        if (value < 0) {
            break;
        }
        buffer[count++] = static_cast<char>(value);
    }
    return count;
}

size_t Stream::readBytesUntil(char terminator, char *buffer, size_t length)
{
    size_t count = 0;
    while (count < length) {
        int value = timedRead();
        if (value < 0 || value == terminator) {
            break;
        }
        buffer[count++] = static_cast<char>(value);
    }
    return count;
}

String Stream::readString()
{
    String result;
    for (int value = timedRead(); value >= 0; value = timedRead()) {
        result += static_cast<char>(value);
    }
    return result;
}

String Stream::readStringUntil(char terminator)
{
    String result;
    for (int value = timedRead(); value >= 0 && value != terminator; value = timedRead()) {
        result += static_cast<char>(value);
    }
    return result;
}
