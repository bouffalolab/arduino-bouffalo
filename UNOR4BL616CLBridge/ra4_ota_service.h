#ifndef UNOR4_BL616CL_RA4_OTA_SERVICE_H
#define UNOR4_BL616CL_RA4_OTA_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "ra4_ota_container.h"

namespace bridge {

struct Ra4OtaCacheBackend {
    bool (*begin)(void *context, const char *path, bool format_on_fail);
    bool (*write)(void *context, const uint8_t *data, size_t size);
    bool (*commit)(void *context);
    void (*discard)(void *context);
    size_t (*maximum_output_size)(void *context);
    void *context;
};

class Ra4OtaService {
public:
    enum class Error : int {
        None = 0,
        StorageInit = -3,
        StorageEnd = -4,
        UrlParse = -5,
        ServerConnect = -6,
        HttpHeader = -7,
        ParseHttpHeader = -8,
        HeaderLength = -9,
        HeaderCrc = -10,
        HeaderMagic = -11,
        Download = -12,
        HeaderTimeout = -13,
        HttpResponse = -14,
    };

    enum class State : uint8_t {
        Idle,
        Receiving,
        Ready,
        Failed,
    };

    explicit Ra4OtaService(const Ra4OtaCacheBackend &cache);

    Error begin(const char *path, bool format_on_fail);
    Error feed(const uint8_t *data, size_t size);
    Error finish();
    Error abort(Error error);
    void reset();

    State state() const;
    Error error() const;
    size_t decoded_size() const;

private:
    static bool write_cache(void *context, const uint8_t *data, size_t size);
    Error fail(Error error);
    static Error map_error(Ra4OtaContainer::Error error);

    Ra4OtaCacheBackend cache_;
    Ra4OtaContainer container_;
    State state_ = State::Idle;
    Error error_ = Error::None;
};

} // namespace bridge

#endif
