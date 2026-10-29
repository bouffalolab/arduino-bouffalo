#ifndef UNOR4_BL616CL_PING_SERVICE_H
#define UNOR4_BL616CL_PING_SERVICE_H

#include <stdint.h>

#include "at_transport.h"

namespace bridge {

enum class PingResult : int {
    DestinationUnreachable = -1,
    Timeout = -2,
    UnknownHost = -3,
    Error = -4,
    Success = 1,
};

struct PingBackend {
    void *context = nullptr;
    bool (*resolve_hostname)(
        void *context,
        const char *hostname,
        uint8_t address[4]) = nullptr;
    PingResult (*echo)(
        void *context,
        const uint8_t address[4],
        uint8_t ttl,
        uint8_t count,
        uint32_t *average_rtt_ms) = nullptr;
};

class PingService {
public:
    explicit PingService(PingBackend backend = {});

    bool handles_command(const at::Command &command) const;
    at::CommandResult handle_command(
        const at::Command &command,
        at::AtTransport &transport);

private:
    bool parse_uint8(
        const char *text,
        unsigned long maximum,
        uint8_t &value) const;

    PingBackend backend_;
};

const PingBackend &bouffalo_ping_backend();

} // namespace bridge

#endif
