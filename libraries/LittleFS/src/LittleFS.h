#pragma once
#include <Arduino.h>
extern "C" {
#include "lfs.h"
}

#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"

class LittleFSClass;
namespace fs {
class File : public Stream {
public:
    File() = default;
    File(const File &other);
    File &operator=(const File &other);
    ~File();
    operator bool() const;
    using Print::write;
    size_t write(uint8_t value) override { return write(&value, 1); }
    size_t write(const uint8_t *data, size_t size) override;
    int read() override;
    int read(uint8_t *data, size_t size);
    int available() override;
    int peek() override;
    void flush() override { sync(); }
    bool sync();
    bool close();
    bool seek(uint32_t position);
    size_t position();
    size_t size();
    int lastError() const;
private:
    struct Handle;
    Handle *_handle = nullptr;
    void release();
    friend class ::LittleFSClass;
};
}
using fs::File;

// All File and filesystem calls must be serialized by the caller in task context.
class LittleFSClass {
public:
    bool begin();                    // Never formats on mount failure.
    bool format();                   // Explicit; refuses a mounted filesystem.
    bool end();                      // Refuses while a file remains open.
    File open(const char *path, const char *mode = FILE_READ);
    bool exists(const char *path);
    bool remove(const char *path);
    bool rename(const char *from, const char *to);
    bool mkdir(const char *path);
    bool rmdir(const char *path);
    size_t totalBytes() const;
    size_t usedBytes();               // Allocated blocks, not logical file bytes.
    int lastError() const { return _error; }
    bool mounted() const { return _mounted; }
private:
    uint32_t _address = 0;
    uint32_t _capacity = 0;
    lfs_t _fs = {};
    lfs_config _config = {};
    bool _mounted = false;
    unsigned _openFiles = 0;
    int _error = 0;
    bool configure();
    bool pathValid(const char *path);
    friend class fs::File;
};
extern LittleFSClass LittleFS;
