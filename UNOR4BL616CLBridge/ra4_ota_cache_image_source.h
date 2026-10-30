#ifndef UNOR4_BL616CL_RA4_OTA_CACHE_IMAGE_SOURCE_H
#define UNOR4_BL616CL_RA4_OTA_CACHE_IMAGE_SOURCE_H

#include <stddef.h>
#include <stdint.h>

#include "ra4_samba_programmer.h"

namespace bridge {

/*
 * Read-only file hooks used after an OTA container has been decoded and
 * committed. The cache source records the committed path and exact decoded
 * size, so a later programming request cannot select an arbitrary LittleFS
 * file or silently accept a changed cache file.
 */
struct Ra4OtaCacheImageReader {
    bool (*open)(void *context, const char *path, size_t *size);
    bool (*read)(
        void *context,
        size_t offset,
        uint8_t *destination,
        size_t size);
    void (*close)(void *context);
    void *context;
};

class Ra4OtaCacheImageSource {
public:
    static constexpr size_t kPathCapacity = 256U;

    explicit Ra4OtaCacheImageSource(const Ra4OtaCacheImageReader &reader = {});

    void set_reader(const Ra4OtaCacheImageReader &reader);
    bool mark_committed(const char *path, size_t size);
    void invalidate();
    bool ready(const char *path = nullptr) const;
    const char *path() const;
    size_t size() const;
    Ra4SambaImageSource image_source();

private:
    static bool open_source(void *context, size_t *size);
    static bool read_source(
        void *context,
        size_t offset,
        uint8_t *destination,
        size_t size);
    static void close_source(void *context);

    Ra4OtaCacheImageReader reader_ = {};
    char path_[kPathCapacity] = {};
    size_t size_ = 0U;
    bool committed_ = false;
    bool open_ = false;
};

} // namespace bridge

#endif
