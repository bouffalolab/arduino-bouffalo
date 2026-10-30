#include "ping_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace bridge {

namespace {

constexpr char kPing[] = "+PING";

void write_result(
    at::AtTransport &transport,
    const at::Command &command,
    int result)
{
    char text[16] = {};
    const int size = snprintf(text, sizeof(text), "%d", result);
    if (size > 0) {
        (void)transport.write_response(command, text);
    }
}

} // namespace

PingService::PingService(PingBackend backend)
    : backend_(backend)
{
}

bool PingService::handles_command(const at::Command &command) const
{
    return strcmp(command.name, kPing) == 0;
}

bool PingService::parse_uint8(
    const char *text,
    unsigned long maximum,
    uint8_t &value) const
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    char *end = nullptr;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed > maximum) {
        return false;
    }
    value = static_cast<uint8_t>(parsed);
    return true;
}

at::CommandResult PingService::handle_command(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 4U ||
        command.arguments[1][0] == '\0' ||
        backend_.resolve_hostname == nullptr ||
        backend_.echo == nullptr) {
        return at::CommandResult::Error;
    }

    uint8_t ttl = 0U;
    uint8_t count = 0U;
    if (!parse_uint8(command.arguments[2], 255U, ttl) ||
        !parse_uint8(command.arguments[3], 255U, count)) {
        return at::CommandResult::Error;
    }
    if (count == 0U) {
        count = 1U;
    }

    uint8_t address[4] = {};
    if (!backend_.resolve_hostname(backend_.context, command.arguments[1], address)) {
        write_result(
            transport,
            command,
            static_cast<int>(PingResult::UnknownHost));
        return at::CommandResult::Ok;
    }

    uint32_t average_rtt_ms = 0U;
    const PingResult result = backend_.echo(
        backend_.context,
        address,
        ttl,
        count,
        &average_rtt_ms);
    write_result(
        transport,
        command,
        result == PingResult::Success
            ? static_cast<int>(average_rtt_ms)
            : static_cast<int>(result));
    return at::CommandResult::Ok;
}

} // namespace bridge
