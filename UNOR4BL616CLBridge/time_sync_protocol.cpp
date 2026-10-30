#include "time_sync_protocol.h"

namespace bridge {

namespace {

uint32_t read_be32(const uint8_t data[4])
{
    return (static_cast<uint32_t>(data[0]) << 24U) |
        (static_cast<uint32_t>(data[1]) << 16U) |
        (static_cast<uint32_t>(data[2]) << 8U) |
        static_cast<uint32_t>(data[3]);
}

} // namespace

bool parse_ntp_unix_epoch(
    const uint8_t *packet,
    size_t size,
    uint32_t *epoch_seconds)
{
    if (packet == nullptr || epoch_seconds == nullptr ||
        size < kNtpPacketBytes) {
        return false;
    }

    const uint32_t ntp_seconds =
        read_be32(packet + kNtpTransmitTimestampOffset);
    if (ntp_seconds < kNtpUnixEpochOffset) {
        return false;
    }

    const uint32_t unix_seconds = ntp_seconds - kNtpUnixEpochOffset;
    if (unix_seconds < kUnixEpochYear2000) {
        return false;
    }

    *epoch_seconds = unix_seconds;
    return true;
}

bool unix_epoch_to_utc(uint32_t epoch_seconds, UtcDateTime *utc)
{
    if (utc == nullptr || epoch_seconds < kUnixEpochYear2000) {
        return false;
    }

    const uint32_t day_seconds = 24U * 60U * 60U;
    const uint32_t days_since_epoch = epoch_seconds / day_seconds;
    const uint32_t seconds_of_day = epoch_seconds % day_seconds;

    /*
     * Howard Hinnant's civil-date conversion, with 1970-01-01 moved to
     * the algorithm's 0000-03-01 epoch. It is timezone independent.
     */
    int64_t z = static_cast<int64_t>(days_since_epoch) + 719468LL;
    const int64_t era = (z >= 0 ? z : z - 146096LL) / 146097LL;
    const unsigned doe = static_cast<unsigned>(z - era * 146097LL);
    const unsigned yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
    int year = static_cast<int>(yoe) + static_cast<int>(era * 400LL);
    const unsigned doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
    const unsigned month_prime = (5U * doy + 2U) / 153U;
    const unsigned day = doy - (153U * month_prime + 2U) / 5U + 1U;
    const unsigned month = month_prime + (month_prime < 10U ? 3U : -9);
    year += month <= 2U ? 1 : 0;

    utc->year = year;
    utc->month = static_cast<uint8_t>(month);
    utc->day = static_cast<uint8_t>(day);
    utc->hour = static_cast<uint8_t>(seconds_of_day / 3600U);
    utc->minute = static_cast<uint8_t>((seconds_of_day % 3600U) / 60U);
    utc->second = static_cast<uint8_t>(seconds_of_day % 60U);
    utc->weekday = static_cast<uint8_t>((days_since_epoch + 4U) % 7U);
    return true;
}

} // namespace bridge
