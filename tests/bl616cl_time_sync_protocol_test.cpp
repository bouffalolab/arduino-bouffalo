#include "../UNOR4BL616CLBridge/time_sync_protocol.h"

#include <cstdlib>
#include <iostream>

namespace {

[[noreturn]] void fail(const char *message)
{
    std::cerr << "test failure: " << message << '\n';
    std::exit(1);
}

void expect(bool value, const char *message)
{
    if (!value) {
        fail(message);
    }
}

void write_be32(uint8_t output[4], uint32_t value)
{
    output[0] = static_cast<uint8_t>(value >> 24U);
    output[1] = static_cast<uint8_t>(value >> 16U);
    output[2] = static_cast<uint8_t>(value >> 8U);
    output[3] = static_cast<uint8_t>(value);
}

} // namespace

int main()
{
    uint8_t packet[bridge::kNtpPacketBytes] = {};
    uint32_t epoch = 0U;
    expect(
        !bridge::parse_ntp_unix_epoch(packet, sizeof(packet) - 1U, &epoch),
        "truncated NTP response is rejected");
    expect(
        !bridge::parse_ntp_unix_epoch(nullptr, sizeof(packet), &epoch),
        "null NTP response is rejected");

    write_be32(
        packet + bridge::kNtpTransmitTimestampOffset,
        bridge::kNtpUnixEpochOffset + bridge::kUnixEpochYear2000);
    expect(
        bridge::parse_ntp_unix_epoch(packet, sizeof(packet), &epoch) &&
            epoch == bridge::kUnixEpochYear2000,
        "NTP Y2K response maps to Unix epoch");

    bridge::UtcDateTime utc = {};
    expect(
        bridge::unix_epoch_to_utc(bridge::kUnixEpochYear2000, &utc) &&
            utc.year == 2000 && utc.month == 1U && utc.day == 1U &&
            utc.hour == 0U && utc.minute == 0U && utc.second == 0U &&
            utc.weekday == 6U,
        "Y2K Unix epoch converts to UTC");
    expect(
        bridge::unix_epoch_to_utc(1709251200UL, &utc) &&
            utc.year == 2024 && utc.month == 3U && utc.day == 1U &&
            utc.hour == 0U && utc.minute == 0U && utc.second == 0U,
        "leap-year date converts to UTC");
    expect(
        !bridge::unix_epoch_to_utc(bridge::kUnixEpochYear2000 - 1U, &utc),
        "pre-2000 epoch is not published");
    return 0;
}
