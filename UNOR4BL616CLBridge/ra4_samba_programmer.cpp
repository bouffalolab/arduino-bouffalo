#include "ra4_samba_programmer.h"

#include <stdio.h>
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
constexpr uint32_t kApplicationFlashAddress = 0U;
constexpr uint32_t kStagingAddress = 0x34U;
constexpr uint32_t kAppletWordsAddress = 0x30U;
constexpr uint32_t kAppletStackAddress = 0x20U;
constexpr char kExpectedVersionPrefix[] =
    "Arduino Bootloader (SAM-BA extended) ";

constexpr uint8_t kWordCopyApplet[] = {
    0x09U, 0x48U, 0x0AU, 0x49U, 0x0AU, 0x4AU, 0x02U, 0xE0U,
    0x08U, 0xC9U, 0x08U, 0xC0U, 0x01U, 0x3AU, 0x00U, 0x2AU,
    0xFAU, 0xD1U, 0x04U, 0x48U, 0x00U, 0x28U, 0x01U, 0xD1U,
    0x01U, 0x48U, 0x85U, 0x46U, 0x70U, 0x47U, 0xC0U, 0x46U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U,
};

static_assert(
    sizeof(kWordCopyApplet) == kStagingAddress,
    "SAM-BA staging buffer must immediately follow the compatibility applet");

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

Ra4SambaProgrammer::Ra4SambaProgrammer(
    Ra4BootstrapControl &bootstrap,
    const Ra4SambaLink &link,
    const Ra4SambaUserUartHooks &user_uart)
    : bootstrap_(bootstrap), link_(link), user_uart_(user_uart)
{
}

bool Ra4SambaProgrammer::request_program(const Ra4SambaImageSource &source)
{
    if (active() || request_pending_ || source.open == nullptr ||
        source.read == nullptr || source.close == nullptr) {
        return false;
    }
    source_ = source;
    request_pending_ = true;
    return true;
}

void Ra4SambaProgrammer::poll(uint32_t now_ms)
{
    if (request_pending_ && !active()) {
        request_pending_ = false;
        start(now_ms);
    }

    switch (diagnostic_.state) {
    case Ra4SambaProgrammerState::kIdle:
        return;
    case Ra4SambaProgrammerState::kWaitBootloader:
        if (!deadline_reached(now_ms, deadline_ms_)) {
            return;
        }
        if (!write(&kAutoBaudByte, 1U)) {
            fail(Ra4SambaProgrammerError::kWrite, now_ms);
            return;
        }
        diagnostic_.state = Ra4SambaProgrammerState::kWaitAutoBaudSecond;
        deadline_ms_ = now_ms + kAutoBaudSettleMs;
        return;
    case Ra4SambaProgrammerState::kWaitAutoBaudSecond:
        if (!deadline_reached(now_ms, deadline_ms_)) {
            return;
        }
        if (!write(&kAutoBaudByte, 1U)) {
            fail(Ra4SambaProgrammerError::kWrite, now_ms);
            return;
        }
        diagnostic_.state = Ra4SambaProgrammerState::kWaitAutoBaudConfirm;
        deadline_ms_ = now_ms + kAutoBaudSettleMs;
        return;
    case Ra4SambaProgrammerState::kWaitAutoBaudConfirm:
        if (!deadline_reached(now_ms, deadline_ms_)) {
            return;
        }
        if (!write(&kAutoBaudConfirm, 1U)) {
            fail(Ra4SambaProgrammerError::kWrite, now_ms);
            return;
        }
        if (link_.clear_rx != nullptr) {
            link_.clear_rx(link_.context);
        }
        if (!write(kBinaryMode, sizeof(kBinaryMode))) {
            fail(Ra4SambaProgrammerError::kWrite, now_ms);
            return;
        }
        binary_reply_index_ = 0U;
        diagnostic_.state = Ra4SambaProgrammerState::kWaitBinaryReply;
        deadline_ms_ = now_ms + kReplyTimeoutMs;
        return;
    case Ra4SambaProgrammerState::kWaitBinaryReply:
        consume_binary_reply(now_ms);
        return;
    case Ra4SambaProgrammerState::kWaitVersionReply:
        consume_version_reply(now_ms);
        return;
    case Ra4SambaProgrammerState::kSendAppletPayload:
        send_applet_payload(now_ms);
        return;
    case Ra4SambaProgrammerState::kWaitAppletStaged:
        if (deadline_reached(now_ms, deadline_ms_)) {
            send_applet_words(now_ms);
        }
        return;
    case Ra4SambaProgrammerState::kWaitAppletWords:
        if (deadline_reached(now_ms, deadline_ms_)) {
            send_applet_stack(now_ms);
        }
        return;
    case Ra4SambaProgrammerState::kWaitAppletStack:
        if (deadline_reached(now_ms, deadline_ms_)) {
            erase_application(now_ms);
        }
        return;
    case Ra4SambaProgrammerState::kWaitEraseReply:
        consume_short_reply(
            'X',
            Ra4SambaProgrammerState::kWaitChunkStaged,
            Ra4SambaProgrammerError::kEraseReplyTimeout,
            Ra4SambaProgrammerError::kEraseReplyInvalid,
            now_ms);
        return;
    case Ra4SambaProgrammerState::kWaitChunkStaged:
        if (deadline_reached(now_ms, deadline_ms_)) {
            select_staging_buffer(now_ms);
        }
        return;
    case Ra4SambaProgrammerState::kSendChunkPayload:
        send_chunk_payload(now_ms);
        return;
    case Ra4SambaProgrammerState::kWaitSelectBufferReply:
        consume_short_reply(
            'Y',
            Ra4SambaProgrammerState::kWaitWriteChunkReply,
            Ra4SambaProgrammerError::kSelectReplyTimeout,
            Ra4SambaProgrammerError::kSelectReplyInvalid,
            now_ms);
        return;
    case Ra4SambaProgrammerState::kWaitWriteChunkReply:
        consume_short_reply(
            'Y',
            Ra4SambaProgrammerState::kWaitChecksumReply,
            Ra4SambaProgrammerError::kWriteReplyTimeout,
            Ra4SambaProgrammerError::kWriteReplyInvalid,
            now_ms);
        return;
    case Ra4SambaProgrammerState::kWaitChecksumReply:
        /*
         * Kept as a telemetry ABI value for older diagnostic clients. New
         * sessions never enter it because this UART path has no usable Z
         * response; recover safely if stale state is ever observed.
         */
        fail(Ra4SambaProgrammerError::kChecksumReplyInvalid, now_ms);
        return;
    case Ra4SambaProgrammerState::kWaitApplication:
        if (deadline_reached(now_ms, deadline_ms_)) {
            finish(diagnostic_.error == Ra4SambaProgrammerError::kNone, now_ms);
        }
        return;
    case Ra4SambaProgrammerState::kRecovery:
        finish(false, now_ms);
        return;
    }
}

bool Ra4SambaProgrammer::active() const
{
    return diagnostic_.active;
}

bool Ra4SambaProgrammer::busy() const
{
    return active() || request_pending_;
}

const Ra4SambaProgrammerDiagnostic &Ra4SambaProgrammer::diagnostic() const
{
    return diagnostic_;
}

bool Ra4SambaProgrammer::write(const uint8_t *data, size_t size)
{
    if (link_.write == nullptr || data == nullptr || size == 0U) {
        return false;
    }
    const size_t written = link_.write(link_.context, data, size);
    diagnostic_.transmitted_bytes += static_cast<uint32_t>(written);
    return written == size;
}

bool Ra4SambaProgrammer::write_cstr(const char *text)
{
    return text != nullptr &&
        write(reinterpret_cast<const uint8_t *>(text), strlen(text));
}

bool Ra4SambaProgrammer::set_user_uart_baud(uint32_t baud)
{
    return link_.update_baud != nullptr &&
        link_.update_baud(link_.context, baud);
}

void Ra4SambaProgrammer::start(uint32_t now_ms)
{
    ++diagnostic_.sequence;
    diagnostic_.state = Ra4SambaProgrammerState::kIdle;
    diagnostic_.error = Ra4SambaProgrammerError::kNone;
    diagnostic_.active = true;
    diagnostic_.succeeded = false;
    diagnostic_.started_at_ms = now_ms;
    diagnostic_.completed_at_ms = 0U;
    diagnostic_.transmitted_bytes = 0U;
    diagnostic_.received_bytes = 0U;
    diagnostic_.image_bytes = 0U;
    diagnostic_.verified_bytes = 0U;
    diagnostic_.chunk_index = 0U;
    memset(diagnostic_.version, 0, sizeof(diagnostic_.version));
    image_size_ = 0U;
    image_offset_ = 0U;
    chunk_payload_size_ = 0U;
    chunk_program_size_ = 0U;
    chunk_stage_offset_ = 0U;
    chunk_flash_address_ = kApplicationFlashAddress;
    binary_reply_index_ = 0U;
    binary_reply_noise_count_ = 0U;
    short_reply_index_ = 0U;
    version_size_ = 0U;
    bootloader_session_ = false;
    recovery_command_sent_ = false;
    source_open_ = false;

    if (!source_.open(source_.context, &image_size_)) {
        fail(Ra4SambaProgrammerError::kImageSource, now_ms);
        return;
    }
    source_open_ = true;
    if (image_size_ == 0U || image_size_ > kApplicationFlashBytes) {
        fail(Ra4SambaProgrammerError::kImageSize, now_ms);
        return;
    }
    diagnostic_.image_bytes = static_cast<uint32_t>(image_size_);

    if (user_uart_.suspend_bridge == nullptr ||
        !user_uart_.suspend_bridge(user_uart_.context)) {
        fail(Ra4SambaProgrammerError::kSuspendUserUart, now_ms);
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
        fail(Ra4SambaProgrammerError::kEnterProgramMode, now_ms);
        return;
    }
    if (!set_user_uart_baud(kSambaBaud)) {
        fail(Ra4SambaProgrammerError::kSetSambaBaud, now_ms);
        return;
    }
    bootloader_session_ = true;
    diagnostic_.state = Ra4SambaProgrammerState::kWaitBootloader;
    deadline_ms_ = now_ms + kBootloaderSettleMs;
}

void Ra4SambaProgrammer::fail(
    Ra4SambaProgrammerError error,
    uint32_t now_ms)
{
    if (diagnostic_.error == Ra4SambaProgrammerError::kNone) {
        diagnostic_.error = error;
    }
    begin_recovery(now_ms);
}

void Ra4SambaProgrammer::finish(bool succeeded, uint32_t now_ms)
{
    close_source();
    const bool restored = set_user_uart_baud(user_uart_baud_);
    if (!restored && diagnostic_.error == Ra4SambaProgrammerError::kNone) {
        diagnostic_.error = Ra4SambaProgrammerError::kRestoreUserUart;
    }
    const bool released = bootstrap_.release();
    if (!released && diagnostic_.error == Ra4SambaProgrammerError::kNone) {
        diagnostic_.error = Ra4SambaProgrammerError::kReleaseBootstrap;
    }
    const bool resumed = user_uart_.resume_bridge != nullptr &&
        user_uart_.resume_bridge(user_uart_.context);
    if (!resumed && diagnostic_.error == Ra4SambaProgrammerError::kNone) {
        diagnostic_.error = Ra4SambaProgrammerError::kSuspendUserUart;
    }
    diagnostic_.succeeded = succeeded && restored && released && resumed &&
        diagnostic_.error == Ra4SambaProgrammerError::kNone;
    diagnostic_.active = false;
    diagnostic_.state = Ra4SambaProgrammerState::kIdle;
    diagnostic_.completed_at_ms = now_ms;
}

void Ra4SambaProgrammer::begin_recovery(uint32_t now_ms)
{
    if (bootloader_session_ && !recovery_command_sent_) {
        (void)write(kRunRequest, sizeof(kRunRequest));
        recovery_command_sent_ = true;
    }
    diagnostic_.state = Ra4SambaProgrammerState::kRecovery;
    deadline_ms_ = now_ms;
}

void Ra4SambaProgrammer::consume_binary_reply(uint32_t now_ms)
{
    while (link_.read != nullptr) {
        const int value = link_.read(link_.context);
        if (value < 0) {
            break;
        }
        ++diagnostic_.received_bytes;
        const uint8_t byte = static_cast<uint8_t>(value);
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
            fail(Ra4SambaProgrammerError::kBinaryReplyInvalid, now_ms);
            return;
        }
        fail(Ra4SambaProgrammerError::kBinaryReplyInvalid, now_ms);
        return;
    }
    if (deadline_reached(now_ms, deadline_ms_)) {
        fail(Ra4SambaProgrammerError::kBinaryReplyTimeout, now_ms);
    }
}

void Ra4SambaProgrammer::consume_version_reply(uint32_t now_ms)
{
    while (link_.read != nullptr) {
        const int value = link_.read(link_.context);
        if (value < 0) {
            break;
        }
        ++diagnostic_.received_bytes;
        if (version_size_ + 1U >= sizeof(diagnostic_.version)) {
            fail(Ra4SambaProgrammerError::kVersionReplyInvalid, now_ms);
            return;
        }
        diagnostic_.version[version_size_++] = static_cast<char>(value);
        diagnostic_.version[version_size_] = '\0';
        if (version_reply_complete(diagnostic_.version, version_size_)) {
            if (strncmp(
                    diagnostic_.version,
                    kExpectedVersionPrefix,
                    strlen(kExpectedVersionPrefix)) != 0) {
                fail(Ra4SambaProgrammerError::kVersionReplyInvalid, now_ms);
                return;
            }
            if (!has_required_capabilities()) {
                fail(Ra4SambaProgrammerError::kMissingCapabilities, now_ms);
                return;
            }
            stage_applet(now_ms);
            return;
        }
    }
    if (deadline_reached(now_ms, deadline_ms_)) {
        fail(Ra4SambaProgrammerError::kVersionReplyTimeout, now_ms);
    }
}

void Ra4SambaProgrammer::consume_short_reply(
    uint8_t expected,
    Ra4SambaProgrammerState next,
    Ra4SambaProgrammerError timeout_error,
    Ra4SambaProgrammerError invalid_error,
    uint32_t now_ms)
{
    static constexpr uint8_t kTerminator[] = {'\n', '\r'};
    while (link_.read != nullptr) {
        const int value = link_.read(link_.context);
        if (value < 0) {
            break;
        }
        ++diagnostic_.received_bytes;
        const uint8_t actual = static_cast<uint8_t>(value);
        const uint8_t required = short_reply_index_ == 0U
            ? expected
            : kTerminator[short_reply_index_ - 1U];
        if (actual != required) {
            fail(invalid_error, now_ms);
            return;
        }
        ++short_reply_index_;
        if (short_reply_index_ == 3U) {
            short_reply_index_ = 0U;
            if (next == Ra4SambaProgrammerState::kWaitChunkStaged) {
                if (!prepare_next_chunk(now_ms)) {
                    fail(Ra4SambaProgrammerError::kAppletSource, now_ms);
                    return;
                }
            } else if (next == Ra4SambaProgrammerState::kWaitWriteChunkReply) {
                write_staged_chunk(now_ms);
            } else if (next == Ra4SambaProgrammerState::kWaitChecksumReply) {
                complete_staged_chunk(now_ms);
            }
            return;
        }
    }
    if (deadline_reached(now_ms, deadline_ms_)) {
        fail(timeout_error, now_ms);
    }
}

void Ra4SambaProgrammer::send_version_request(uint32_t now_ms)
{
    if (!write(kVersionRequest, sizeof(kVersionRequest))) {
        fail(Ra4SambaProgrammerError::kWrite, now_ms);
        return;
    }
    version_size_ = 0U;
    diagnostic_.state = Ra4SambaProgrammerState::kWaitVersionReply;
    deadline_ms_ = now_ms + kReplyTimeoutMs;
}

void Ra4SambaProgrammer::stage_applet(uint32_t now_ms)
{
    char command[32] = {};
    const int length = snprintf(
        command,
        sizeof(command),
        "S%08lX,%08lX#",
        0UL,
        static_cast<unsigned long>(sizeof(kWordCopyApplet)));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(command) ||
        !write_cstr(command)) {
        fail(Ra4SambaProgrammerError::kWrite, now_ms);
        return;
    }
    diagnostic_.state = Ra4SambaProgrammerState::kSendAppletPayload;
    deadline_ms_ = now_ms + kPayloadCommandSettleMs;
}

void Ra4SambaProgrammer::send_applet_payload(uint32_t now_ms)
{
    if (!deadline_reached(now_ms, deadline_ms_)) {
        return;
    }
    if (!write(kWordCopyApplet, sizeof(kWordCopyApplet))) {
        fail(Ra4SambaProgrammerError::kWrite, now_ms);
        return;
    }
    diagnostic_.state = Ra4SambaProgrammerState::kWaitAppletStaged;
    deadline_ms_ = now_ms + kCommandDataSettleMs;
}

void Ra4SambaProgrammer::send_applet_words(uint32_t now_ms)
{
    char command[32] = {};
    const int length = snprintf(
        command,
        sizeof(command),
        "W%08lX,%08lX#",
        static_cast<unsigned long>(kAppletWordsAddress),
        static_cast<unsigned long>(kStagingBytes / sizeof(uint32_t)));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(command) ||
        !write_cstr(command)) {
        fail(Ra4SambaProgrammerError::kWrite, now_ms);
        return;
    }
    diagnostic_.state = Ra4SambaProgrammerState::kWaitAppletWords;
    deadline_ms_ = now_ms + kCommandDataSettleMs;
}

void Ra4SambaProgrammer::send_applet_stack(uint32_t now_ms)
{
    char command[32] = {};
    const int length = snprintf(
        command,
        sizeof(command),
        "W%08lX,%08lX#",
        static_cast<unsigned long>(kAppletStackAddress),
        0UL);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(command) ||
        !write_cstr(command)) {
        fail(Ra4SambaProgrammerError::kWrite, now_ms);
        return;
    }
    diagnostic_.state = Ra4SambaProgrammerState::kWaitAppletStack;
    deadline_ms_ = now_ms + kCommandDataSettleMs;
}

void Ra4SambaProgrammer::erase_application(uint32_t now_ms)
{
    if (!write_cstr("X00000000#")) {
        fail(Ra4SambaProgrammerError::kWrite, now_ms);
        return;
    }
    short_reply_index_ = 0U;
    diagnostic_.state = Ra4SambaProgrammerState::kWaitEraseReply;
    deadline_ms_ = now_ms + kLongReplyTimeoutMs;
}

void Ra4SambaProgrammer::stage_next_chunk(uint32_t now_ms)
{
    char command[32] = {};
    const int length = snprintf(
        command,
        sizeof(command),
        "S%08lX,%08lX#",
        static_cast<unsigned long>(kStagingAddress),
        static_cast<unsigned long>(chunk_program_size_));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(command) ||
        !write_cstr(command)) {
        fail(Ra4SambaProgrammerError::kWrite, now_ms);
        return;
    }
    chunk_stage_offset_ = 0U;
    diagnostic_.state = Ra4SambaProgrammerState::kSendChunkPayload;
    deadline_ms_ = now_ms + kPayloadCommandSettleMs;
}

void Ra4SambaProgrammer::send_chunk_payload(uint32_t now_ms)
{
    if (!deadline_reached(now_ms, deadline_ms_)) {
        return;
    }
    if (chunk_stage_offset_ >= chunk_program_size_) {
        diagnostic_.state = Ra4SambaProgrammerState::kWaitChunkStaged;
        deadline_ms_ = now_ms + kCommandDataSettleMs;
        return;
    }

    const size_t remaining = chunk_program_size_ - chunk_stage_offset_;
    const size_t packet_size = remaining < kPayloadPacketBytes
        ? remaining
        : kPayloadPacketBytes;
    if (!write(staging_ + chunk_stage_offset_, packet_size)) {
        fail(Ra4SambaProgrammerError::kWrite, now_ms);
        return;
    }
    chunk_stage_offset_ += packet_size;
    diagnostic_.state = chunk_stage_offset_ == chunk_program_size_
        ? Ra4SambaProgrammerState::kWaitChunkStaged
        : Ra4SambaProgrammerState::kSendChunkPayload;
    deadline_ms_ = now_ms + (
        chunk_stage_offset_ == chunk_program_size_
            ? kCommandDataSettleMs
            : kPayloadInterpacketSettleMs);
}

void Ra4SambaProgrammer::select_staging_buffer(uint32_t now_ms)
{
    if (!write_cstr("Y00000034,0#")) {
        fail(Ra4SambaProgrammerError::kWrite, now_ms);
        return;
    }
    short_reply_index_ = 0U;
    diagnostic_.state = Ra4SambaProgrammerState::kWaitSelectBufferReply;
    deadline_ms_ = now_ms + kReplyTimeoutMs;
}

void Ra4SambaProgrammer::write_staged_chunk(uint32_t now_ms)
{
    char command[40] = {};
    const int length = snprintf(
        command,
        sizeof(command),
        "Y%08lX,%08lX#",
        static_cast<unsigned long>(chunk_flash_address_),
        static_cast<unsigned long>(chunk_program_size_));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(command) ||
        !write_cstr(command)) {
        fail(Ra4SambaProgrammerError::kWrite, now_ms);
        return;
    }
    short_reply_index_ = 0U;
    diagnostic_.state = Ra4SambaProgrammerState::kWaitWriteChunkReply;
    deadline_ms_ = now_ms + kLongReplyTimeoutMs;
}

void Ra4SambaProgrammer::complete_staged_chunk(uint32_t now_ms)
{
    /*
     * The RA4 extended bootloader advertises Z but does not return Z (or R)
     * replies over this UART bridge. Arduino BOSSA's -e -w -R update path
     * relies on each completed Y reply and the restarted application instead.
     * The OTA gate verifies the resulting RA4 application by its unique
     * runtime marker.
     */
    diagnostic_.verified_bytes += static_cast<uint32_t>(chunk_payload_size_);
    image_offset_ += chunk_payload_size_;
    ++diagnostic_.chunk_index;
    if (image_offset_ == image_size_) {
        if (!write(kRunRequest, sizeof(kRunRequest))) {
            fail(Ra4SambaProgrammerError::kWrite, now_ms);
            return;
        }
        recovery_command_sent_ = true;
        diagnostic_.state = Ra4SambaProgrammerState::kWaitApplication;
        deadline_ms_ = now_ms + kApplicationSettleMs;
        return;
    }
    if (!prepare_next_chunk(now_ms)) {
        fail(Ra4SambaProgrammerError::kAppletSource, now_ms);
    }
}

bool Ra4SambaProgrammer::prepare_next_chunk(uint32_t now_ms)
{
    if (image_offset_ >= image_size_ || source_.read == nullptr) {
        return false;
    }
    const size_t remaining = image_size_ - image_offset_;
    chunk_payload_size_ = remaining < kStagingBytes ? remaining : kStagingBytes;
    chunk_program_size_ =
        (chunk_payload_size_ + kFlashPageBytes - 1U) / kFlashPageBytes * kFlashPageBytes;
    if (chunk_program_size_ > sizeof(staging_) ||
        !source_.read(
            source_.context,
            image_offset_,
            staging_,
            chunk_payload_size_)) {
        return false;
    }
    if (chunk_program_size_ > chunk_payload_size_) {
        memset(
            staging_ + chunk_payload_size_,
            0,
            chunk_program_size_ - chunk_payload_size_);
    }
    chunk_flash_address_ =
        kApplicationFlashAddress + static_cast<uint32_t>(image_offset_);
    stage_next_chunk(now_ms);
    return diagnostic_.error == Ra4SambaProgrammerError::kNone;
}

bool Ra4SambaProgrammer::has_required_capabilities() const
{
    return strstr(diagnostic_.version, "[Arduino:") != nullptr &&
        strchr(diagnostic_.version, 'X') != nullptr &&
        strchr(diagnostic_.version, 'Y') != nullptr &&
        strchr(diagnostic_.version, 'Z') != nullptr &&
        strchr(diagnostic_.version, 'K') != nullptr;
}

void Ra4SambaProgrammer::close_source()
{
    if (source_open_) {
        source_.close(source_.context);
        source_open_ = false;
    }
}

} // namespace bridge
