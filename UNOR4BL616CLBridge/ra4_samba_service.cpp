#include "ra4_samba_service.h"

#include <string.h>

namespace bridge {
namespace {

constexpr uint8_t kAutoBaudByte = 0x80U;
constexpr uint8_t kAutoBaudConfirm = '#';
constexpr uint8_t kBinaryMode[] = {'N', '#'};
constexpr uint8_t kBinaryReply[] = {'\n', '\r'};
constexpr uint8_t kMaximumBinaryReplyNoiseBytes = 32U;
constexpr uint8_t kVersionRequest[] = {'V', '#'};
constexpr uint8_t kRunRequest[] = {'K', '#'};
constexpr char kExpectedVersionPrefix[] =
    "Arduino Bootloader (SAM-BA extended) ";

bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

bool version_reply_complete(const char *version, size_t size)
{
    if (size < 2U) {
        return false;
    }

    const char first = version[size - 2U];
    const char second = version[size - 1U];
    return (first == '\r' && second == '\n') ||
        (first == '\n' && second == '\r');
}

} // namespace

Ra4SambaService::Ra4SambaService(
    Ra4BootstrapControl &bootstrap,
    const Ra4SambaLink &link,
    const Ra4SambaUserUartHooks &user_uart)
    : bootstrap_(bootstrap), link_(link), user_uart_(user_uart)
{
}

bool Ra4SambaService::request_read_only_handshake()
{
    if (active() || request_pending_) {
        return false;
    }
    request_pending_ = true;
    return true;
}

void Ra4SambaService::poll(uint32_t now_ms)
{
    if (request_pending_ && !active()) {
        request_pending_ = false;
        start(now_ms);
    }

    switch (diagnostic_.state) {
    case Ra4SambaState::kIdle:
        return;
    case Ra4SambaState::kWaitBootloader:
        if (!deadline_reached(now_ms, deadline_ms_)) {
            return;
        }
        if (!write(&kAutoBaudByte, 1U)) {
            fail(Ra4SambaError::kWrite, now_ms);
            return;
        }
        diagnostic_.state = Ra4SambaState::kWaitAutoBaudSecond;
        deadline_ms_ = now_ms + kAutoBaudSettleMs;
        return;
    case Ra4SambaState::kWaitAutoBaudSecond:
        if (!deadline_reached(now_ms, deadline_ms_)) {
            return;
        }
        if (!write(&kAutoBaudByte, 1U)) {
            fail(Ra4SambaError::kWrite, now_ms);
            return;
        }
        diagnostic_.state = Ra4SambaState::kWaitAutoBaudConfirm;
        deadline_ms_ = now_ms + kAutoBaudSettleMs;
        return;
    case Ra4SambaState::kWaitAutoBaudConfirm:
        if (!deadline_reached(now_ms, deadline_ms_)) {
            return;
        }
        if (!write(&kAutoBaudConfirm, 1U)) {
            fail(Ra4SambaError::kWrite, now_ms);
            return;
        }
        if (link_.clear_rx != nullptr) {
            link_.clear_rx(link_.context);
        }
        if (!write(kBinaryMode, sizeof(kBinaryMode))) {
            fail(Ra4SambaError::kWrite, now_ms);
            return;
        }
        binary_reply_index_ = 0U;
        diagnostic_.state = Ra4SambaState::kWaitBinaryReply;
        deadline_ms_ = now_ms + kReplyTimeoutMs;
        return;
    case Ra4SambaState::kWaitBinaryReply:
        consume_binary_reply(now_ms);
        return;
    case Ra4SambaState::kWaitVersionReply:
        consume_version_reply(now_ms);
        return;
    case Ra4SambaState::kWaitApplication:
        if (deadline_reached(now_ms, deadline_ms_)) {
            finish(diagnostic_.error == Ra4SambaError::kNone, now_ms);
        }
        return;
    case Ra4SambaState::kRecovery:
        finish(false, now_ms);
        return;
    }
}

bool Ra4SambaService::active() const
{
    return diagnostic_.active;
}

bool Ra4SambaService::busy() const
{
    return active() || request_pending_;
}

const Ra4SambaDiagnostic &Ra4SambaService::diagnostic() const
{
    return diagnostic_;
}

bool Ra4SambaService::write(const uint8_t *data, size_t size)
{
    if (link_.write == nullptr || data == nullptr || size == 0U) {
        return false;
    }
    const size_t written = link_.write(link_.context, data, size);
    diagnostic_.transmitted_bytes += static_cast<uint32_t>(written);
    return written == size;
}

bool Ra4SambaService::set_user_uart_baud(uint32_t baud)
{
    return link_.update_baud != nullptr &&
        link_.update_baud(link_.context, baud);
}

void Ra4SambaService::start(uint32_t now_ms)
{
    ++diagnostic_.sequence;
    diagnostic_.state = Ra4SambaState::kIdle;
    diagnostic_.error = Ra4SambaError::kNone;
    diagnostic_.active = true;
    diagnostic_.succeeded = false;
    diagnostic_.started_at_ms = now_ms;
    diagnostic_.completed_at_ms = 0U;
    diagnostic_.transmitted_bytes = 0U;
    diagnostic_.received_bytes = 0U;
    diagnostic_.binary_reply_bytes[0] = 0U;
    diagnostic_.binary_reply_bytes[1] = 0U;
    memset(diagnostic_.version, 0, sizeof(diagnostic_.version));
    binary_reply_index_ = 0U;
    binary_reply_capture_size_ = 0U;
    binary_reply_noise_count_ = 0U;
    version_size_ = 0U;
    recovery_command_sent_ = false;
    bootloader_session_ = false;

    if (user_uart_.suspend_bridge == nullptr ||
        !user_uart_.suspend_bridge(user_uart_.context)) {
        fail(Ra4SambaError::kSuspendUserUart, now_ms);
        return;
    }

    user_uart_baud_ = user_uart_.current_baud != nullptr
        ? user_uart_.current_baud(user_uart_.context)
        : kDefaultUserUartBaud;
    if (user_uart_baud_ == 0U || user_uart_baud_ == 1200U || user_uart_baud_ == 2400U) {
        user_uart_baud_ = kDefaultUserUartBaud;
    }

    if (link_.clear_rx != nullptr) {
        link_.clear_rx(link_.context);
    }
    if (!bootstrap_.enter_program_mode()) {
        fail(Ra4SambaError::kEnterProgramMode, now_ms);
        return;
    }
    if (!set_user_uart_baud(kSambaBaud)) {
        fail(Ra4SambaError::kSetSambaBaud, now_ms);
        return;
    }

    bootloader_session_ = true;
    diagnostic_.state = Ra4SambaState::kWaitBootloader;
    deadline_ms_ = now_ms + kBootloaderSettleMs;
}

void Ra4SambaService::fail(Ra4SambaError error, uint32_t now_ms)
{
    if (diagnostic_.error == Ra4SambaError::kNone) {
        diagnostic_.error = error;
    }
    begin_recovery(now_ms);
}

void Ra4SambaService::finish(bool succeeded, uint32_t now_ms)
{
    bool restored = set_user_uart_baud(user_uart_baud_);
    if (!restored && diagnostic_.error == Ra4SambaError::kNone) {
        diagnostic_.error = Ra4SambaError::kRestoreUserUart;
    }
    const bool released = bootstrap_.release();
    if (!released && diagnostic_.error == Ra4SambaError::kNone) {
        diagnostic_.error = Ra4SambaError::kReleaseBootstrap;
    }
    bool resumed = false;
    if (user_uart_.resume_bridge != nullptr) {
        resumed = user_uart_.resume_bridge(user_uart_.context);
    }
    if (!resumed && diagnostic_.error == Ra4SambaError::kNone) {
        diagnostic_.error = Ra4SambaError::kSuspendUserUart;
    }
    diagnostic_.succeeded = succeeded && restored && released && resumed &&
        diagnostic_.error == Ra4SambaError::kNone;
    diagnostic_.active = false;
    diagnostic_.state = Ra4SambaState::kIdle;
    diagnostic_.completed_at_ms = now_ms;
}

void Ra4SambaService::begin_recovery(uint32_t now_ms)
{
    if (bootloader_session_ && !recovery_command_sent_) {
        (void)write(kRunRequest, sizeof(kRunRequest));
        recovery_command_sent_ = true;
    }
    diagnostic_.state = Ra4SambaState::kRecovery;
    deadline_ms_ = now_ms;
}

void Ra4SambaService::consume_binary_reply(uint32_t now_ms)
{
    while (link_.read != nullptr) {
        const int value = link_.read(link_.context);
        if (value < 0) {
            break;
        }
        ++diagnostic_.received_bytes;
        const uint8_t byte = static_cast<uint8_t>(value);
        if (binary_reply_capture_size_ < sizeof(diagnostic_.binary_reply_bytes)) {
            diagnostic_.binary_reply_bytes[binary_reply_capture_size_++] =
                static_cast<uint8_t>(value);
        }
        if (byte == kBinaryReply[binary_reply_index_]) {
            ++binary_reply_index_;
            if (binary_reply_index_ == sizeof(kBinaryReply)) {
                if (link_.clear_rx != nullptr) {
                    link_.clear_rx(link_.context);
                }
                send_version_request(now_ms);
                return;
            }
            continue;
        }
        if (binary_reply_index_ == 0U &&
            ++binary_reply_noise_count_ <= kMaximumBinaryReplyNoiseBytes) {
            continue;
        }
        if (binary_reply_index_ != 0U && byte == kBinaryReply[0]) {
            continue;
        }
        if (binary_reply_index_ == 0U) {
            fail(Ra4SambaError::kBinaryReplyInvalid, now_ms);
            return;
        }
        fail(Ra4SambaError::kBinaryReplyInvalid, now_ms);
        return;
    }

    if (deadline_reached(now_ms, deadline_ms_)) {
        fail(Ra4SambaError::kBinaryReplyTimeout, now_ms);
    }
}

void Ra4SambaService::consume_version_reply(uint32_t now_ms)
{
    while (link_.read != nullptr) {
        const int value = link_.read(link_.context);
        if (value < 0) {
            break;
        }
        ++diagnostic_.received_bytes;
        if (version_size_ + 1U >= sizeof(diagnostic_.version)) {
            fail(Ra4SambaError::kVersionReplyInvalid, now_ms);
            return;
        }
        diagnostic_.version[version_size_++] = static_cast<char>(value);
        diagnostic_.version[version_size_] = '\0';
        if (version_reply_complete(diagnostic_.version, version_size_)) {
            if (strncmp(
                    diagnostic_.version,
                    kExpectedVersionPrefix,
                    strlen(kExpectedVersionPrefix)) != 0) {
                fail(Ra4SambaError::kVersionReplyInvalid, now_ms);
                return;
            }
            if (!write(kRunRequest, sizeof(kRunRequest))) {
                fail(Ra4SambaError::kWrite, now_ms);
                return;
            }
            recovery_command_sent_ = true;
            diagnostic_.state = Ra4SambaState::kWaitApplication;
            deadline_ms_ = now_ms + kApplicationSettleMs;
            return;
        }
    }

    if (deadline_reached(now_ms, deadline_ms_)) {
        fail(Ra4SambaError::kVersionReplyTimeout, now_ms);
    }
}

void Ra4SambaService::send_version_request(uint32_t now_ms)
{
    if (!write(kVersionRequest, sizeof(kVersionRequest))) {
        fail(Ra4SambaError::kWrite, now_ms);
        return;
    }
    version_size_ = 0U;
    diagnostic_.state = Ra4SambaState::kWaitVersionReply;
    deadline_ms_ = now_ms + kReplyTimeoutMs;
}

} // namespace bridge
