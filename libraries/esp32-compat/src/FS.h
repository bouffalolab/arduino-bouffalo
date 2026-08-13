#ifndef BL616CL_ESP32_COMPAT_FS_H_
#define BL616CL_ESP32_COMPAT_FS_H_

#include <Arduino.h>
#include <WString.h>

#define FILE_READ  "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"

enum SeekMode {
    SeekSet = 0,
    SeekCur = 1,
    SeekEnd = 2
};

class File {
public:
    File() {}

    operator bool() const { return false; }
    size_t write(uint8_t value) { (void)value; return 0; }
    size_t write(const uint8_t *buffer, size_t size) { (void)buffer; (void)size; return 0; }
    int available() { return 0; }
    int read() { return -1; }
    int read(uint8_t *buffer, size_t size) { (void)buffer; (void)size; return 0; }
    int peek() { return -1; }
    void flush() {}
    void close() {}
    String name() { return String(""); }
    size_t size() { return 0; }
    bool seek(uint32_t position) { (void)position; return false; }
    bool seek(uint32_t position, SeekMode mode) { (void)position; (void)mode; return false; }
    size_t position() { return 0; }
};

#endif
