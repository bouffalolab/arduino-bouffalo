#ifndef BL616CL_ESP32_COMPAT_FS_H_
#define BL616CL_ESP32_COMPAT_FS_H_

#include <Arduino.h>
#include <WString.h>
#include <memory>

#define FILE_READ   "r"
#define FILE_WRITE  "w"
#define FILE_APPEND "a"

enum SeekMode {
    SeekSet = 0,
    SeekCur = 1,
    SeekEnd = 2
};

class FS;

/*
 * Opaque handle wrapper. The lfs types (lfs_t/lfs_file_t/lfs_dir_t) are
 * deliberately kept out of this header; FS.cpp casts the void* back and
 * owns allocation/deallocation of the concrete objects.
 */
struct FileImpl {
    void *lfs = nullptr;      /* lfs_t* */
    void *file = nullptr;     /* lfs_file_t* */
    void *dir = nullptr;      /* lfs_dir_t* */
    bool open = false;
    bool is_dir = false;
    String full_path;
    String entry_name;

    ~FileImpl();
};

class File {
public:
    File();
    explicit File(std::shared_ptr<FileImpl> impl);

    operator bool() const;
    size_t write(uint8_t value);
    size_t write(const uint8_t *buffer, size_t size);
    int available();
    int read();
    int read(uint8_t *buffer, size_t size);
    int peek();
    void flush();
    void close();
    String name() const;
    String path() const;
    size_t size();
    bool seek(uint32_t position);
    bool seek(uint32_t position, SeekMode mode);
    size_t position();

    bool isDirectory();
    File openNextFile();
    void rewindDirectory();

private:
    std::shared_ptr<FileImpl> _impl;
};

class FS {
public:
    virtual ~FS() {}
    virtual File open(const char *path, const char *mode = FILE_READ) = 0;
    virtual bool exists(const char *path) = 0;
    virtual bool remove(const char *path) = 0;
    virtual bool rename(const char *from, const char *to) = 0;
    virtual bool mkdir(const char *path) = 0;
    virtual bool rmdir(const char *path) = 0;
};

#endif
