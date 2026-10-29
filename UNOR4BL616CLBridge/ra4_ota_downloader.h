#ifndef UNOR4_BL616CL_RA4_OTA_DOWNLOADER_H
#define UNOR4_BL616CL_RA4_OTA_DOWNLOADER_H

#include <stddef.h>
#include <stdint.h>

#include "ra4_ota_service.h"

namespace bridge {

enum class Ra4OtaDownloadReadResult : uint8_t {
    Data,
    Pending,
    Closed,
    Error,
};

struct Ra4OtaDownloadBackend {
    void *context = nullptr;
    bool (*open)(
        void *context,
        bool secure,
        const char *hostname,
        uint16_t port,
        const uint8_t *ca_certificate,
        size_t ca_certificate_size,
        uint32_t timeout_ms,
        int *handle) = nullptr;
    bool (*write)(
        void *context,
        int handle,
        const uint8_t *data,
        size_t size) = nullptr;
    Ra4OtaDownloadReadResult (*read)(
        void *context,
        int handle,
        uint8_t *data,
        size_t capacity,
        size_t *size) = nullptr;
    void (*close)(void *context, int handle) = nullptr;
};

enum class Ra4OtaDownloadTimeoutReason : uint8_t {
    None,
    Header,
    Body,
};

struct Ra4OtaDownloadDiagnostic {
    Ra4OtaDownloadReadResult last_read_result =
        Ra4OtaDownloadReadResult::Pending;
    Ra4OtaDownloadTimeoutReason timeout_reason =
        Ra4OtaDownloadTimeoutReason::None;
    bool secure = false;
    bool last_read_valid = false;
    size_t last_read_size = 0U;
    size_t content_length = 0U;
    size_t downloaded_size = 0U;
    uint32_t poll_count = 0U;
    uint32_t data_count = 0U;
    uint32_t pending_count = 0U;
    uint32_t closed_count = 0U;
    uint32_t error_count = 0U;
    uint32_t last_progress_ms = 0U;
    uint32_t last_poll_ms = 0U;
};

struct Ra4OtaDownloadBackendDiagnostic {
    bool open = false;
    bool secure = false;
    int handle = -1;
    size_t last_available = 0U;
    size_t last_receive_size = 0U;
    bool last_receive_succeeded = false;
    uint32_t available_call_count = 0U;
    uint32_t available_ready_count = 0U;
    uint32_t available_pending_count = 0U;
    uint32_t available_failure_count = 0U;
    uint32_t receive_call_count = 0U;
    uint32_t receive_failure_count = 0U;
};

class Ra4OtaDownloader {
public:
    enum class State : uint8_t {
        Idle,
        ReceivingHeaders,
        ReceivingBody,
        Ready,
        Failed,
    };

    explicit Ra4OtaDownloader(
        Ra4OtaService &ota,
        const Ra4OtaDownloadBackend &backend);

    Ra4OtaService::Error start(
        const char *url,
        const uint8_t *ca_certificate,
        size_t ca_certificate_size,
        uint32_t now_ms);
    void poll(uint32_t now_ms);
    void cancel();

    State state() const;
    Ra4OtaService::Error error() const;
    size_t content_length() const;
    size_t downloaded_size() const;
    Ra4OtaDownloadDiagnostic diagnostic() const;

private:
    static constexpr size_t kHostnameCapacity = 192U;
    static constexpr size_t kPathCapacity = 384U;
    static constexpr size_t kRequestCapacity = 704U;
    static constexpr size_t kHeaderCapacity = 2048U;
    static constexpr size_t kReadBufferCapacity = 512U;
    static constexpr uint32_t kHeaderTimeoutMs = 10000U;
    /*
     * The ESP32-S3 baseline keeps polling a started download until the
     * transfer completes or its socket reports an error. A short per-read
     * timeout breaks valid throttled streams on the final carrier, where
     * Wi-Fi/LittleFS scheduling can leave multi-second gaps between readable
     * TLS records. Keep recovery finite without rejecting normal OTA peers.
     */
    static constexpr uint32_t kBodyTimeoutMs = 30000U;

    struct ParsedUrl {
        bool secure = false;
        uint16_t port = 0U;
        char hostname[kHostnameCapacity] = {};
        char path[kPathCapacity] = {};
    };

    bool parse_url(const char *url, ParsedUrl &parsed) const;
    bool write_request(const ParsedUrl &url);
    bool append_header(const uint8_t *data, size_t size, size_t *consumed);
    bool parse_response_header();
    bool feed_body(const uint8_t *data, size_t size);
    bool header_complete() const;
    bool timed_out(uint32_t now_ms) const;
    void close_connection();
    void fail(Ra4OtaService::Error error);

    Ra4OtaService &ota_;
    const Ra4OtaDownloadBackend backend_;
    State state_ = State::Idle;
    Ra4OtaService::Error error_ = Ra4OtaService::Error::None;
    int handle_ = -1;
    size_t content_length_ = 0U;
    size_t downloaded_size_ = 0U;
    uint32_t last_progress_ms_ = 0U;
    uint32_t last_poll_ms_ = 0U;
    uint32_t poll_count_ = 0U;
    uint32_t data_count_ = 0U;
    uint32_t pending_count_ = 0U;
    uint32_t closed_count_ = 0U;
    uint32_t error_count_ = 0U;
    size_t last_read_size_ = 0U;
    bool secure_ = false;
    bool last_read_valid_ = false;
    Ra4OtaDownloadReadResult last_read_result_ =
        Ra4OtaDownloadReadResult::Pending;
    Ra4OtaDownloadTimeoutReason timeout_reason_ =
        Ra4OtaDownloadTimeoutReason::None;
    Ra4OtaService::Error header_error_ = Ra4OtaService::Error::ParseHttpHeader;
    uint8_t header_[kHeaderCapacity] = {};
    size_t header_size_ = 0U;
    uint8_t read_buffer_[kReadBufferCapacity] = {};
};

const Ra4OtaDownloadBackend &bouffalo_ra4_ota_download_backend();
Ra4OtaDownloadBackendDiagnostic
bouffalo_ra4_ota_download_backend_diagnostic_snapshot();

} // namespace bridge

#endif
