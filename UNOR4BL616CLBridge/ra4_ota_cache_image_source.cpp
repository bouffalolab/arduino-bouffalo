#include "ra4_ota_cache_image_source.h"

#include <string.h>

namespace bridge {

Ra4OtaCacheImageSource::Ra4OtaCacheImageSource(
    const Ra4OtaCacheImageReader &reader)
    : reader_(reader)
{
}

void Ra4OtaCacheImageSource::set_reader(
    const Ra4OtaCacheImageReader &reader)
{
    if (open_) {
        close_source(this);
    }
    reader_ = reader;
    invalidate();
}

bool Ra4OtaCacheImageSource::mark_committed(
    const char *path,
    size_t size)
{
    if (open_ || path == nullptr || path[0] == '\0' || size == 0U ||
        strlen(path) >= sizeof(path_)) {
        return false;
    }
    memcpy(path_, path, strlen(path) + 1U);
    size_ = size;
    committed_ = true;
    return true;
}

void Ra4OtaCacheImageSource::invalidate()
{
    if (open_) {
        close_source(this);
    }
    path_[0] = '\0';
    size_ = 0U;
    committed_ = false;
}

bool Ra4OtaCacheImageSource::ready(const char *path) const
{
    return committed_ && !open_ && path_[0] != '\0' && size_ != 0U &&
        (path == nullptr || strcmp(path_, path) == 0);
}

const char *Ra4OtaCacheImageSource::path() const
{
    return committed_ ? path_ : nullptr;
}

size_t Ra4OtaCacheImageSource::size() const
{
    return committed_ ? size_ : 0U;
}

Ra4SambaImageSource Ra4OtaCacheImageSource::image_source()
{
    return {
        open_source,
        read_source,
        close_source,
        this,
    };
}

bool Ra4OtaCacheImageSource::open_source(
    void *context,
    size_t *size)
{
    Ra4OtaCacheImageSource *source =
        static_cast<Ra4OtaCacheImageSource *>(context);
    if (source == nullptr || size == nullptr || !source->ready() ||
        source->reader_.open == nullptr || source->reader_.read == nullptr ||
        source->reader_.close == nullptr) {
        return false;
    }
    size_t actual_size = 0U;
    if (!source->reader_.open(
            source->reader_.context,
            source->path_,
            &actual_size) ||
        actual_size != source->size_) {
        if (actual_size != source->size_ && source->reader_.close != nullptr) {
            source->reader_.close(source->reader_.context);
        }
        return false;
    }
    source->open_ = true;
    *size = actual_size;
    return true;
}

bool Ra4OtaCacheImageSource::read_source(
    void *context,
    size_t offset,
    uint8_t *destination,
    size_t size)
{
    Ra4OtaCacheImageSource *source =
        static_cast<Ra4OtaCacheImageSource *>(context);
    return source != nullptr && source->open_ && destination != nullptr &&
        offset <= source->size_ && size <= source->size_ - offset &&
        source->reader_.read != nullptr &&
        source->reader_.read(
            source->reader_.context,
            offset,
            destination,
            size);
}

void Ra4OtaCacheImageSource::close_source(void *context)
{
    Ra4OtaCacheImageSource *source =
        static_cast<Ra4OtaCacheImageSource *>(context);
    if (source == nullptr || !source->open_) {
        return;
    }
    if (source->reader_.close != nullptr) {
        source->reader_.close(source->reader_.context);
    }
    source->open_ = false;
}

} // namespace bridge
