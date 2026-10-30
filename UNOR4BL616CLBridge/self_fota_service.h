#ifndef UNOR4_BL616CL_SELF_FOTA_SERVICE_H
#define UNOR4_BL616CL_SELF_FOTA_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "at_transport.h"

namespace bridge {

/*
 * Small sequential frontend for the Bouffalo A/B firmware updater.
 *
 * The payload is the post-processed BL616CL application image followed by
 * its 32-byte SHA-256 digest; the first raw chunk includes the BFNP magic.
 * RA4M1 OTA remains a separate service and
 * never calls this class.
 */
class SelfFotaService {
public:
    bool handles_command(const at::Command &command) const;
    at::CommandResult handle_command(
        const at::Command &command,
        at::AtTransport &transport);

    bool take_reset_request();
    bool active() const { return active_; }

private:
    static bool parse_u32(const char *text, uint32_t &value);
    static at::CommandResult receive_chunk(
        void *context,
        const uint8_t *data,
        size_t size);
    at::CommandResult begin(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult data(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult finish(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult abort(
        const at::Command &command,
        at::AtTransport &transport);

    uint32_t total_size_ = 0U;
    uint32_t offset_ = 0U;
    bool active_ = false;
    bool reset_request_ = false;
};

} // namespace bridge

#endif
