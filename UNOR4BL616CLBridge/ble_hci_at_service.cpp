#include "ble_hci_at_service.h"

#include <string.h>

namespace bridge {

namespace {

constexpr char kHciBeginCommand[] = "+HCIBEGIN";
constexpr char kHciEndCommand[] = "+HCIEND";
constexpr char kHciWaitCommand[] = "+HCIWAIT";
constexpr char kHciReadCommand[] = "+HCIREAD";
constexpr char kHciWriteCommand[] = "+HCIWRITE";
constexpr char kHciAvailableCommand[] = "+HCIAVAILABLE";
constexpr size_t kHciReadChunkBytes = 258U;

bool command_is(const at::Command &command, const char *name, at::CommandMode mode)
{
    return command.mode == mode && strcmp(command.name, name) == 0;
}

} // namespace

BleHciAtService::BleHciAtService(BleHciService &hci, BleHciWaitHooks wait_hooks)
    : hci_(hci), wait_hooks_(wait_hooks)
{
}

at::CommandResult BleHciAtService::handle_command(const at::Command &command, at::AtTransport &transport)
{
    if (command_is(command, kHciBeginCommand, at::CommandMode::Run)) {
        /*
         * ArduinoBLE can restart after the RA4M1 resets without issuing
         * HCIEND for the previous transport. Treat every HCIBEGIN as a
         * fresh session so scan state and queued advertising reports cannot
         * leak into the next Reset command.
         */
        if (hci_.active()) {
            hci_.end();
        }
        if (!hci_.begin() || !write_prompt(command, transport)) {
            return at::CommandResult::Error;
        }
        return at::CommandResult::Ok;
    }

    if (command_is(command, kHciEndCommand, at::CommandMode::Run)) {
        hci_.end();
        return write_prompt(command, transport) ? at::CommandResult::Ok : at::CommandResult::Error;
    }

    if (command_is(command, kHciWaitCommand, at::CommandMode::Write)) {
        uint32_t timeout_ms = 0U;
        if (command.argument_count != 1U ||
            !parse_unsigned(command.arguments[0], BleHciService::kMaxWaitMs, timeout_ms)) {
            return at::CommandResult::Error;
        }

        const BleHciWaitResult result = hci_.wait_for_packet(timeout_ms, wait_hooks_);
        if (result == BleHciWaitResult::Inactive || result == BleHciWaitResult::Invalid ||
            !write_prompt(command, transport)) {
            return at::CommandResult::Error;
        }
        return at::CommandResult::Ok;
    }

    if (command_is(command, kHciAvailableCommand, at::CommandMode::Read)) {
        if (!hci_.active() ||
            !transport.write_cstr(command.name) ||
            !transport.write_cstr(": ") ||
            !write_unsigned(transport, hci_.available()) ||
            !transport.write_cstr("\r\n")) {
            return at::CommandResult::Error;
        }
        return at::CommandResult::Ok;
    }

    if (command_is(command, kHciReadCommand, at::CommandMode::Run)) {
        if (!hci_.active()) {
            return at::CommandResult::Error;
        }

        const size_t available = hci_.available();
        const size_t response_size =
            available < kHciReadChunkBytes ? available : kHciReadChunkBytes;
        if (!transport.write_cstr(command.name) ||
            !transport.write_cstr(": ") ||
            !write_unsigned(transport, response_size) ||
            !transport.write_cstr("|")) {
            return at::CommandResult::Error;
        }

        uint8_t chunk[64] = {};
        size_t remaining = response_size;
        while (remaining != 0U) {
            const size_t requested = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
            const size_t read = hci_.read(chunk, requested);
            if (read == 0U || !transport.write_data(chunk, read)) {
                return at::CommandResult::Error;
            }
            remaining -= read;
        }
        return at::CommandResult::Ok;
    }

    if (command_is(command, kHciWriteCommand, at::CommandMode::Write)) {
        uint32_t packet_size = 0U;
        if (!hci_.active() ||
            command.argument_count != 1U ||
            !parse_unsigned(command.arguments[0], BleHciService::kMaxPacketBytes, packet_size) ||
            packet_size == 0U ||
            !transport.request_raw_payload(packet_size, &BleHciAtService::handle_raw_write, this)) {
            return at::CommandResult::Error;
        }
        return at::CommandResult::Deferred;
    }

    return at::CommandResult::Error;
}

at::CommandResult BleHciAtService::handle_raw_write(void *context, const uint8_t *data, size_t size)
{
    BleHciAtService *service = static_cast<BleHciAtService *>(context);
    return service->hci_.send_packet(data, size) ? at::CommandResult::Ok : at::CommandResult::Error;
}

bool BleHciAtService::parse_unsigned(const char *text, uint32_t maximum, uint32_t &value)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }

    uint32_t parsed = 0U;
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }

        const uint32_t digit = static_cast<uint32_t>(*cursor - '0');
        if (parsed > (maximum - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }

    value = parsed;
    return true;
}

bool BleHciAtService::write_unsigned(at::AtTransport &transport, size_t value)
{
    char digits[sizeof(size_t) * 3U] = {};
    size_t count = 0U;

    do {
        digits[count++] = static_cast<char>('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));

    while (count != 0U) {
        --count;
        if (!transport.write_data(&digits[count], 1U)) {
            return false;
        }
    }
    return true;
}

bool BleHciAtService::write_prompt(const at::Command &command, at::AtTransport &transport)
{
    return transport.write_cstr(command.name) && transport.write_cstr(": ");
}

} // namespace bridge
