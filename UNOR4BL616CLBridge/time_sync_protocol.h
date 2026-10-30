#ifndef UNOR4_BL616CL_TIME_SYNC_PROTOCOL_H
#define UNOR4_BL616CL_TIME_SYNC_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

namespace bridge {

constexpr uint32_t kUnixEpochYear2000 = 946684800UL;
constexpr size_t kNtpPacketBytes = 48U;
constexpr size_t kNtpTransmitTimestampOffset = 40U;
constexpr uint32_t kNtpUnixEpochOffset = 2208988800UL;

struct UtcDateTime {
    int year = 0;
    uint8_t month = 0U;
    uint8_t day = 0U;
    uint8_t hour = 0U;
    uint8_t minute = 0U;
    uint8_t second = 0U;
    uint8_t weekday = 0U;
};

bool parse_ntp_unix_epoch(
    const uint8_t *packet,
    size_t size,
    uint32_t *epoch_seconds);
bool unix_epoch_to_utc(uint32_t epoch_seconds, UtcDateTime *utc);

} // namespace bridge

#endif
