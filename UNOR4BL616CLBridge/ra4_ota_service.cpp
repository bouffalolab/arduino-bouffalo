#include "ra4_ota_service.h"

namespace bridge {

Ra4OtaService::Ra4OtaService(const Ra4OtaCacheBackend &cache)
    : cache_(cache),
      container_(
          Ra4OtaContainer::kUnoR4Magic,
          cache.maximum_output_size != nullptr
              ? cache.maximum_output_size(cache.context)
              : 0U)
{
}

Ra4OtaService::Error Ra4OtaService::begin(
    const char *path,
    bool format_on_fail)
{
    reset();
    if (path == nullptr || path[0] == '\0' || cache_.begin == nullptr ||
        cache_.write == nullptr || cache_.commit == nullptr ||
        cache_.discard == nullptr || cache_.maximum_output_size == nullptr ||
        cache_.maximum_output_size(cache_.context) == 0U ||
        !cache_.begin(cache_.context, path, format_on_fail)) {
        return fail(Error::StorageInit);
    }

    const Ra4OtaContainer::Error result = container_.begin(
        {
            write_cache,
            this,
        });
    if (result != Ra4OtaContainer::Error::None) {
        return fail(map_error(result));
    }
    state_ = State::Receiving;
    error_ = Error::None;
    return Error::None;
}

Ra4OtaService::Error Ra4OtaService::feed(
    const uint8_t *data,
    size_t size)
{
    if (state_ != State::Receiving) {
        return error_ == Error::None ? Error::Download : error_;
    }
    const Ra4OtaContainer::Error result = container_.feed(data, size);
    return result == Ra4OtaContainer::Error::None
        ? Error::None
        : fail(map_error(result));
}

Ra4OtaService::Error Ra4OtaService::finish()
{
    if (state_ != State::Receiving) {
        return error_ == Error::None ? Error::Download : error_;
    }
    const Ra4OtaContainer::Error result = container_.finish();
    if (result != Ra4OtaContainer::Error::None) {
        return fail(map_error(result));
    }
    if (!cache_.commit(cache_.context)) {
        return fail(Error::StorageEnd);
    }
    state_ = State::Ready;
    return Error::None;
}

Ra4OtaService::Error Ra4OtaService::abort(Error error)
{
    if (error == Error::None) {
        error = Error::Download;
    }
    return fail(error);
}

void Ra4OtaService::reset()
{
    if (state_ == State::Receiving || state_ == State::Failed) {
        if (cache_.discard != nullptr) {
            cache_.discard(cache_.context);
        }
    }
    state_ = State::Idle;
    error_ = Error::None;
}

Ra4OtaService::State Ra4OtaService::state() const
{
    return state_;
}

Ra4OtaService::Error Ra4OtaService::error() const
{
    return error_;
}

size_t Ra4OtaService::decoded_size() const
{
    return container_.decoded_size();
}

bool Ra4OtaService::write_cache(
    void *context,
    const uint8_t *data,
    size_t size)
{
    Ra4OtaService *service = static_cast<Ra4OtaService *>(context);
    return service->cache_.write != nullptr &&
        service->cache_.write(service->cache_.context, data, size);
}

Ra4OtaService::Error Ra4OtaService::fail(Error error)
{
    if (cache_.discard != nullptr) {
        cache_.discard(cache_.context);
    }
    state_ = State::Failed;
    error_ = error;
    return error_;
}

Ra4OtaService::Error Ra4OtaService::map_error(
    Ra4OtaContainer::Error error)
{
    switch (error) {
    case Ra4OtaContainer::Error::None:
        return Error::None;
    case Ra4OtaContainer::Error::HeaderMagic:
        return Error::HeaderMagic;
    case Ra4OtaContainer::Error::HeaderLength:
    case Ra4OtaContainer::Error::OutputOverflow:
        return Error::HeaderLength;
    case Ra4OtaContainer::Error::CrcMismatch:
        return Error::HeaderCrc;
    case Ra4OtaContainer::Error::OutputSink:
        return Error::StorageEnd;
    default:
        return Error::Download;
    }
}

} // namespace bridge
