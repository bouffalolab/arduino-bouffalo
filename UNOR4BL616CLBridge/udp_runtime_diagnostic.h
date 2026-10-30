#ifndef UNOR4_BL616CL_UDP_RUNTIME_DIAGNOSTIC_H
#define UNOR4_BL616CL_UDP_RUNTIME_DIAGNOSTIC_H

#include <stdint.h>

#include "udp_service.h"

namespace bridge {

struct UdpRuntimeWaitHooks {
    void *context = nullptr;
    uint32_t (*now_ms)(void *context) = nullptr;
    void (*idle)(void *context) = nullptr;
};

/*
 * Test-only direct UDP probe. It intentionally bypasses UdpService packet
 * buffering and raw-payload handling while using the same lwIP backend.
 */
class UdpRuntimeDiagnostic {
public:
    UdpRuntimeDiagnostic(
        UdpBackend backend,
        UdpRuntimeWaitHooks wait_hooks = {});

    bool handles_command(const at::Command &command) const;
    at::CommandResult handle_command(
        const at::Command &command,
        at::AtTransport &transport);

private:
    bool parse_uint(
        const char *text,
        unsigned long maximum,
        unsigned long &value) const;
    static bool parse_ipv4(const char *text, uint8_t address[4]);
    uint32_t now_ms() const;
    void idle() const;

    UdpBackend backend_;
    UdpRuntimeWaitHooks wait_hooks_;
};

} // namespace bridge

#endif
