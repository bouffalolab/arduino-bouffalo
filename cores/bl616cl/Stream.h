#ifndef Stream_h
#define Stream_h

#include <inttypes.h>

#include "Print.h"

enum LookaheadMode {
    SKIP_ALL,
    SKIP_NONE,
    SKIP_WHITESPACE,
};

class Stream : public Print
{
public:
    Stream() : timeout_(1000), start_millis_(0) {}

    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;

    int read(uint8_t *buffer, size_t length)
    {
        return static_cast<int>(readBytes(buffer, length));
    }

    void setTimeout(unsigned long timeout) { timeout_ = timeout; }
    unsigned long getTimeout() const { return timeout_; }

    size_t readBytes(char *buffer, size_t length);
    size_t readBytes(uint8_t *buffer, size_t length)
    {
        return readBytes(reinterpret_cast<char *>(buffer), length);
    }
    size_t readBytesUntil(char terminator, char *buffer, size_t length);
    size_t readBytesUntil(char terminator, uint8_t *buffer, size_t length)
    {
        return readBytesUntil(terminator, reinterpret_cast<char *>(buffer), length);
    }
    String readString();
    String readStringUntil(char terminator);

protected:
    int timedRead();
    int timedPeek();

private:
    unsigned long timeout_;
    unsigned long start_millis_;
};

#endif
