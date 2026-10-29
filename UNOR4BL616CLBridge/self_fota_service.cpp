#include "self_fota_service.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <fota/compat/bflb_ota.h>
}

namespace bridge {

namespace {
constexpr char kBegin[] = "+BLFOTABEGIN";
constexpr char kData[] = "+BLFOTADATA";
constexpr char kFinish[] = "+BLFOTAFINISH";
constexpr char kAbort[] = "+BLFOTAABORT";
constexpr uint32_t kMinimumImage = 4096U;
constexpr uint32_t kMaximumImage = 0x168000U;
}

bool SelfFotaService::parse_u32(const char *text, uint32_t &value)
{
    if (!text || *text < '0' || *text > '9') return false;
    char *end = nullptr;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

bool SelfFotaService::handles_command(const at::Command &command) const
{
    return strcmp(command.name, kBegin) == 0 ||
        strcmp(command.name, kData) == 0 ||
        strcmp(command.name, kFinish) == 0 ||
        strcmp(command.name, kAbort) == 0;
}

at::CommandResult SelfFotaService::handle_command(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (strcmp(command.name, kBegin) == 0) return begin(command, transport);
    if (strcmp(command.name, kData) == 0) return data(command, transport);
    if (strcmp(command.name, kFinish) == 0) return finish(command, transport);
    if (strcmp(command.name, kAbort) == 0) return abort(command, transport);
    return at::CommandResult::Error;
}

at::CommandResult SelfFotaService::begin(
    const at::Command &command,
    at::AtTransport &transport)
{
    uint32_t size = 0U;
    if (active_ || command.mode != at::CommandMode::Write ||
        command.argument_count != 1U || !parse_u32(command.arguments[0], size) ||
        size < kMinimumImage || size > kMaximumImage) {
        return at::CommandResult::Error;
    }
    if (bflb_ota_start(size) != 0) return at::CommandResult::Error;
    total_size_ = size;
    offset_ = 0U;
    active_ = true;
    reset_request_ = false;
    return transport.write_response(command, "0")
        ? at::CommandResult::Ok : at::CommandResult::Error;
}

at::CommandResult SelfFotaService::data(
    const at::Command &command,
    at::AtTransport &transport)
{
    uint32_t size = 0U;
    if (!active_ || command.mode != at::CommandMode::Write ||
        command.argument_count != 1U || !parse_u32(command.arguments[0], size) ||
        size == 0U || size > at::kTransportRawPayloadCapacity ||
        offset_ > total_size_ || size > total_size_ - offset_ ||
        !transport.request_raw_payload(size, &SelfFotaService::receive_chunk, this)) {
        return at::CommandResult::Error;
    }
    return at::CommandResult::Deferred;
}

at::CommandResult SelfFotaService::receive_chunk(
    void *context,
    const uint8_t *data,
    size_t size)
{
    SelfFotaService *self = static_cast<SelfFotaService *>(context);
    if (!self || !self->active_ || size == 0U ||
        size > self->total_size_ - self->offset_ ||
        // The first chunk must contain the postprocessed BL616CL boot header.
        (self->offset_ == 0U && (size < 4U || memcmp(data, "BFNP", 4U) != 0)) ||
        bflb_ota_update(self->total_size_, self->offset_,
                        const_cast<uint8_t *>(data), static_cast<uint32_t>(size)) != 0) {
        if (self && self->active_) {
            bflb_ota_abort();
            self->active_ = false;
            self->total_size_ = 0U;
            self->offset_ = 0U;
        }
        return at::CommandResult::Error;
    }
    self->offset_ += static_cast<uint32_t>(size);
    return at::CommandResult::Ok;
}

at::CommandResult SelfFotaService::finish(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (!active_ || command.mode != at::CommandMode::Run ||
        command.argument_count != 0U || offset_ != total_size_) {
        return at::CommandResult::Error;
    }
    // The SDK releases its handle on both success and verification failure.
    // Clear our state as well, so a rejected image can be retried immediately.
    const int result = bflb_ota_finish(1U);
    active_ = false;
    total_size_ = 0U;
    offset_ = 0U;
    if (result != 0) return at::CommandResult::Error;
    reset_request_ = true;
    return transport.write_response(command, "0")
        ? at::CommandResult::Ok : at::CommandResult::Error;
}

at::CommandResult SelfFotaService::abort(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (!active_ || command.mode != at::CommandMode::Run ||
        command.argument_count != 0U) {
        return at::CommandResult::Error;
    }
    bflb_ota_abort();
    active_ = false;
    total_size_ = 0U;
    offset_ = 0U;
    return transport.write_response(command, "0")
        ? at::CommandResult::Ok : at::CommandResult::Error;
}

bool SelfFotaService::take_reset_request()
{
    const bool requested = reset_request_;
    reset_request_ = false;
    return requested;
}

} // namespace bridge
