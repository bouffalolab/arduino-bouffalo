#ifndef UNOR4_BL616CL_TIME_SERVICE_H
#define UNOR4_BL616CL_TIME_SERVICE_H

#include <stdint.h>

namespace bridge {

enum class TimeSyncFailure : uint8_t {
    None = 0U,
    TaskCreate = 1U,
    StationUnavailable = 2U,
    Dns = 3U,
    Socket = 4U,
    SocketOptions = 5U,
    Send = 6U,
    Receive = 7U,
    Packet = 8U,
    Rtc = 9U,
};

struct TimeServiceDiagnostic {
    bool station_ready = false;
    bool sync_running = false;
    bool synced = false;
    TimeSyncFailure last_failure = TimeSyncFailure::None;
    uint32_t got_ip_events = 0U;
    uint32_t task_create_count = 0U;
    uint32_t task_create_failure_count = 0U;
    uint32_t sync_attempt_count = 0U;
    uint32_t dns_failure_count = 0U;
    uint32_t socket_failure_count = 0U;
    uint32_t send_failure_count = 0U;
    uint32_t receive_failure_count = 0U;
    uint32_t packet_failure_count = 0U;
    uint32_t rtc_failure_count = 0U;
    uint32_t success_count = 0U;
    int16_t last_dns_result = 0;
    int16_t last_receive_result = 0;
    uint32_t last_ntp_epoch = 0U;
    uint32_t last_rtc_epoch = 0U;
    uint32_t last_published_epoch = 0U;
    uint16_t last_rtc_year = 0U;
    uint32_t epoch_read_count = 0U;
};

/*
 * Mirrors the ESP32 bridge's configTime("pool.ntp.org") behavior without
 * placing blocking DNS/UDP work on the RA4M1 AT or USB task. A successful
 * STA IPv4 event schedules background NTP synchronization; GETTIME reads
 * the BL616CL RTC.
 */
class TimeService {
public:
    void on_wifi_event(uint16_t code);
    void process();
    uint32_t epoch_seconds();
    TimeServiceDiagnostic diagnostic() const;

private:
    static void sync_task(void *context);
    bool sync_once();

    volatile bool station_ready_ = false;
    volatile bool sync_running_ = false;
    volatile bool synced_ = false;
    volatile uint32_t retry_not_before_ms_ = 0U;
    volatile TimeSyncFailure last_failure_ = TimeSyncFailure::None;
    volatile uint32_t got_ip_events_ = 0U;
    volatile uint32_t task_create_count_ = 0U;
    volatile uint32_t task_create_failure_count_ = 0U;
    volatile uint32_t sync_attempt_count_ = 0U;
    volatile uint32_t dns_failure_count_ = 0U;
    volatile uint32_t socket_failure_count_ = 0U;
    volatile uint32_t send_failure_count_ = 0U;
    volatile uint32_t receive_failure_count_ = 0U;
    volatile uint32_t packet_failure_count_ = 0U;
    volatile uint32_t rtc_failure_count_ = 0U;
    volatile uint32_t success_count_ = 0U;
    volatile int16_t last_dns_result_ = 0;
    volatile int16_t last_receive_result_ = 0;
    volatile uint32_t last_ntp_epoch_ = 0U;
    volatile uint32_t last_rtc_epoch_ = 0U;
    volatile uint32_t last_published_epoch_ = 0U;
    volatile uint16_t last_rtc_year_ = 0U;
    volatile uint32_t epoch_read_count_ = 0U;
};

} // namespace bridge

#endif
