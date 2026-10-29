#include "time_service.h"

#include "time_sync_protocol.h"
#include "wifi_service.h"

#include <Arduino.h>

extern "C" {
#include <FreeRTOS.h>
#include <task.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <bflb_rtc.h>
}

namespace bridge {

namespace {

constexpr char kNtpService[] = "123";
constexpr uint32_t kNtpTimeoutMs = 3000U;
constexpr uint32_t kNtpRetryIntervalMs = 5000U;
constexpr uint16_t kTimeTaskStackWords = 1024U;
constexpr UBaseType_t kTimeTaskPriority = ARDUINO_LOOP_PRIORITY + 1U;
constexpr const char *kNtpHostnames[] = {
    "pool.ntp.org",
    "time.google.com",
    "ntp.aliyun.com",
};

bool set_socket_timeouts(int handle)
{
    const struct timeval timeout = {
        static_cast<long>(kNtpTimeoutMs / 1000U),
        static_cast<long>((kNtpTimeoutMs % 1000U) * 1000U),
    };
    return setsockopt(
               handle,
               SOL_SOCKET,
               SO_RCVTIMEO,
               &timeout,
               sizeof(timeout)) == 0 &&
        setsockopt(
               handle,
               SOL_SOCKET,
               SO_SNDTIMEO,
               &timeout,
               sizeof(timeout)) == 0;
}

bool set_rtc_epoch(uint32_t epoch_seconds)
{
    UtcDateTime utc = {};
    if (!unix_epoch_to_utc(epoch_seconds, &utc)) {
        return false;
    }

    struct bflb_tm rtc = {};
    rtc.tm_year = utc.year - 1900;
    rtc.tm_mon = static_cast<int>(utc.month) - 1;
    rtc.tm_mday = utc.day;
    rtc.tm_hour = utc.hour;
    rtc.tm_min = utc.minute;
    rtc.tm_sec = utc.second;
    rtc.tm_wday = utc.weekday;
    bflb_rtc_set_utc_time(&rtc);
    return true;
}

} // namespace

void TimeService::on_wifi_event(uint16_t code)
{
    if (code == static_cast<uint16_t>(WifiEventCode::GotIp)) {
        station_ready_ = true;
        synced_ = false;
        retry_not_before_ms_ = 0U;
        last_failure_ = TimeSyncFailure::None;
        ++got_ip_events_;
    } else if (code == static_cast<uint16_t>(WifiEventCode::Disconnected)) {
        station_ready_ = false;
    }
}

void TimeService::process()
{
    const uint32_t now = millis();
    if (!station_ready_ || synced_ || sync_running_ ||
        static_cast<int32_t>(now - retry_not_before_ms_) < 0) {
        return;
    }

    sync_running_ = true;
    ++task_create_count_;
    if (xTaskCreate(
            &TimeService::sync_task,
            "ntp_sync",
            kTimeTaskStackWords,
            this,
            kTimeTaskPriority,
            nullptr) != pdPASS) {
        sync_running_ = false;
        retry_not_before_ms_ = now + kNtpRetryIntervalMs;
        ++task_create_failure_count_;
        last_failure_ = TimeSyncFailure::TaskCreate;
    }
}

uint32_t TimeService::epoch_seconds()
{
    ++epoch_read_count_;
    struct bflb_tm utc = {};
    bflb_rtc_get_utc_time(&utc);
    last_rtc_year_ = static_cast<uint16_t>(utc.tm_year + 1900);
    const uint64_t epoch = bflb_rtc_get_utc_timestamp();
    last_rtc_epoch_ = epoch <= UINT32_MAX
        ? static_cast<uint32_t>(epoch)
        : 0U;
    if (utc.tm_year + 1900 < 2000) {
        return 0U;
    }

    if (epoch < kUnixEpochYear2000 || epoch > UINT32_MAX) {
        return 0U;
    }
    last_published_epoch_ = static_cast<uint32_t>(epoch);
    return last_published_epoch_;
}

TimeServiceDiagnostic TimeService::diagnostic() const
{
    TimeServiceDiagnostic result = {};
    result.station_ready = station_ready_;
    result.sync_running = sync_running_;
    result.synced = synced_;
    result.last_failure = last_failure_;
    result.got_ip_events = got_ip_events_;
    result.task_create_count = task_create_count_;
    result.task_create_failure_count = task_create_failure_count_;
    result.sync_attempt_count = sync_attempt_count_;
    result.dns_failure_count = dns_failure_count_;
    result.socket_failure_count = socket_failure_count_;
    result.send_failure_count = send_failure_count_;
    result.receive_failure_count = receive_failure_count_;
    result.packet_failure_count = packet_failure_count_;
    result.rtc_failure_count = rtc_failure_count_;
    result.success_count = success_count_;
    result.last_dns_result = last_dns_result_;
    result.last_receive_result = last_receive_result_;
    result.last_ntp_epoch = last_ntp_epoch_;
    result.last_rtc_epoch = last_rtc_epoch_;
    result.last_published_epoch = last_published_epoch_;
    result.last_rtc_year = last_rtc_year_;
    result.epoch_read_count = epoch_read_count_;
    return result;
}

void TimeService::sync_task(void *context)
{
    TimeService *service = static_cast<TimeService *>(context);
    if (service != nullptr) {
        ++service->sync_attempt_count_;
        const bool synced = service->sync_once();
        service->synced_ = synced;
        if (!synced) {
            service->retry_not_before_ms_ = millis() + kNtpRetryIntervalMs;
        }
        service->sync_running_ = false;
    }
    vTaskDelete(nullptr);
}

bool TimeService::sync_once()
{
    if (!station_ready_) {
        last_failure_ = TimeSyncFailure::StationUnavailable;
        return false;
    }

    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    uint8_t request[kNtpPacketBytes] = {};
    request[0] = 0x1BU;
    uint8_t response[kNtpPacketBytes] = {};

    for (const char *hostname : kNtpHostnames) {
        struct addrinfo *addresses = nullptr;
        const int dns_result =
            getaddrinfo(hostname, kNtpService, &hints, &addresses);
        last_dns_result_ = static_cast<int16_t>(dns_result);
        if (dns_result != 0 || addresses == nullptr) {
            ++dns_failure_count_;
            last_failure_ = TimeSyncFailure::Dns;
            if (addresses != nullptr) {
                freeaddrinfo(addresses);
            }
            continue;
        }

        for (const struct addrinfo *address = addresses;
             address != nullptr;
             address = address->ai_next) {
            const int handle = socket(
                address->ai_family,
                address->ai_socktype,
                address->ai_protocol);
            if (handle < 0) {
                ++socket_failure_count_;
                last_failure_ = TimeSyncFailure::Socket;
                continue;
            }

            if (!set_socket_timeouts(handle)) {
                ++socket_failure_count_;
                last_failure_ = TimeSyncFailure::SocketOptions;
                (void)closesocket(handle);
                continue;
            }
            if (sendto(
                    handle,
                    request,
                    sizeof(request),
                    0,
                    address->ai_addr,
                    address->ai_addrlen) != static_cast<int>(sizeof(request))) {
                ++send_failure_count_;
                last_failure_ = TimeSyncFailure::Send;
                (void)closesocket(handle);
                continue;
            }

            const int received = recvfrom(
                handle,
                response,
                sizeof(response),
                0,
                nullptr,
                nullptr);
            last_receive_result_ = static_cast<int16_t>(received);
            (void)closesocket(handle);
            if (received < static_cast<int>(kNtpPacketBytes)) {
                ++receive_failure_count_;
                last_failure_ = TimeSyncFailure::Receive;
                continue;
            }

            uint32_t epoch = 0U;
            if (!parse_ntp_unix_epoch(
                    response,
                    static_cast<size_t>(received),
                    &epoch)) {
                ++packet_failure_count_;
                last_failure_ = TimeSyncFailure::Packet;
                continue;
            }
            last_ntp_epoch_ = epoch;
            if (!set_rtc_epoch(epoch) ||
                epoch_seconds() < kUnixEpochYear2000) {
                ++rtc_failure_count_;
                last_failure_ = TimeSyncFailure::Rtc;
                continue;
            }

            ++success_count_;
            last_failure_ = TimeSyncFailure::None;
            freeaddrinfo(addresses);
            return true;
        }
        freeaddrinfo(addresses);
    }
    return false;
}

} // namespace bridge
