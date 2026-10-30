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
    /**
     * @brief Construct a File reference; copies share the underlying open handle.
     */
    File() = default;
    /**
     * @brief Construct a File reference; copies share the underlying open handle.
     */
    File(const File &other);
    /**
     * @brief Replace this File reference while maintaining shared-handle ownership.
     */
    File &operator=(const File &other);
    /**
     * @brief Release this reference, closing the handle when its final reference disappears.
     */
    ~File();
    /**
     * @brief Report whether this object currently refers to a usable resource.
     */
    operator bool() const;
    using Print::write;
    /**
     * @brief Write the supplied byte or buffer and return the accepted byte count.
     */
    size_t write(uint8_t value) override { return write(&value, 1); }
    /**
     * @brief Write the supplied byte or buffer and return the accepted byte count.
     */
    size_t write(const uint8_t *data, size_t size) override;
    /**
     * @brief Read from the open file; buffer reads return a count (zero at EOF) or -1 on error;
 * single-byte reads return -1 at EOF or on error.
     */
    int read() override;
    /**
     * @brief Read from the open file; buffer reads return a count (zero at EOF) or -1 on error;
 * single-byte reads return -1 at EOF or on error.
     */
    int read(uint8_t *data, size_t size);
    /**
     * @brief Return the currently readable byte count without consuming input.
     */
    int available() override;
    /**
     * @brief Return the next input byte without consuming it, or -1 when none is available.
     */
    int peek() override;
    /**
     * @brief Commit pending file data and metadata through sync(); inspect lastError() for
     * failure.
     */
    void flush() override { sync(); }
    /**
     * @brief Commit pending file data and metadata; return false and record a littlefs error
     * on failure.
     */
    bool sync();
    /**
     * @brief Close the shared open file and report synchronization/close errors.
     */
    bool close();
    /**
     * @brief Move an open file to an absolute byte offset within the supported signed offset
     * range.
     */
    bool seek(uint32_t position);
    /**
     * @brief Return the open file's current byte offset, or zero on failure.
     */
    size_t position();
    /**
     * @brief Return the file's logical length, or zero on failure.
     */
    size_t size();
    /**
     * @brief Return the most recent operation error for this object.
     */
    int lastError() const;
private:
    struct Handle;
    Handle *_handle = nullptr;
    /**
     * @brief Release this File reference and close/delete its shared handle when the last
     * reference disappears.
     */
    void release();
    friend class ::LittleFSClass;
};
}
using fs::File;

// All File and filesystem calls must be serialized by the caller in task context.
class LittleFSClass {
public:
    /**
     * @brief Mount the validated littlefs partition without formatting on failure.
     */
    bool begin();                    // Never formats on mount failure.
    /**
     * @brief Format the validated littlefs partition only while unmounted; return false on
     * failure.
     *
     * @return true on success; false for an invalid partition, a mounted filesystem or Flash
     *   failure.
     */
    bool format();                   // Explicit; refuses a mounted filesystem.
    /**
     * @brief Unmount littlefs only after all File handles are closed.
     *
     * @return true after unmounting; false while files are open or on an unmount error.
     */
    bool end();                      // Refuses while a file remains open.
    /**
     * @brief Open an absolute path with a supported mode, returning an invalid File on
     * failure.
     *
     * @param path Absolute partition-local path without parent traversal.
     * @param mode One of r, r+, w, w+, a or a+.
     * @return A shared File handle, or an invalid File; inspect lastError() on failure.
     */
    File open(const char *path, const char *mode = FILE_READ);
    /**
     * @brief Report whether the supplied path exists in the mounted filesystem.
     */
    bool exists(const char *path);
    /**
     * @brief Remove a regular file, refusing directories and recording filesystem errors.
     */
    bool remove(const char *path);
    /**
     * @brief Rename a filesystem entry within the mounted partition; return false on failure.
     */
    bool rename(const char *from, const char *to);
    /**
     * @brief Create a directory in the mounted filesystem; return false on failure.
     */
    bool mkdir(const char *path);
    /**
     * @brief Remove an empty directory, recording the filesystem error on failure.
     */
    bool rmdir(const char *path);
    /**
     * @brief Return the mounted filesystem capacity, or zero while unmounted.
     */
    size_t totalBytes() const;
    /**
     * @brief Return allocated filesystem block bytes, recording littlefs errors separately.
     */
    size_t usedBytes();               // Allocated blocks, not logical file bytes.
    /**
     * @brief Return the most recent operation error for this object.
     */
    int lastError() const { return _error; }
    /**
     * @brief Report whether the filesystem is mounted.
     */
    bool mounted() const { return _mounted; }
private:
    uint32_t _address = 0;
    uint32_t _capacity = 0;
    lfs_t _fs = {};
    lfs_config _config = {};
    bool _mounted = false;
    unsigned _openFiles = 0;
    int _error = 0;
    /**
     * @brief Load the selected partition geometry and configure partition-scoped littlefs
     * callbacks.
     */
    bool configure();
    /**
     * @brief Require a mounted filesystem and an absolute path without parent traversal.
     */
    bool pathValid(const char *path);
    friend class fs::File;
};
extern LittleFSClass LittleFS;
