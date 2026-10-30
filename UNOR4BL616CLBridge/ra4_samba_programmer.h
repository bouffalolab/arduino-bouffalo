#ifndef UNOR4_BL616CL_RA4_SAMBA_PROGRAMMER_H
#define UNOR4_BL616CL_RA4_SAMBA_PROGRAMMER_H

#include <stddef.h>
#include <stdint.h>

#include "ra4_samba_service.h"

namespace bridge {

enum class Ra4SambaProgrammerState : uint8_t {
    kIdle = 0U,
    kWaitBootloader = 1U,
    kWaitAutoBaudSecond = 2U,
    kWaitAutoBaudConfirm = 3U,
    kWaitBinaryReply = 4U,
    kWaitVersionReply = 5U,
    kWaitAppletStaged = 6U,
    kWaitAppletWords = 7U,
    kWaitAppletStack = 8U,
    kWaitEraseReply = 9U,
    kWaitChunkStaged = 10U,
    kWaitSelectBufferReply = 11U,
    kWaitWriteChunkReply = 12U,
    kWaitChecksumReply = 13U,
    kWaitApplication = 14U,
    kRecovery = 15U,
    kSendChunkPayload = 16U,
    kSendAppletPayload = 17U,
};

enum class Ra4SambaProgrammerError : uint8_t {
    kNone = 0U,
    kBusy = 1U,
    kImageSource = 2U,
    kImageSize = 3U,
    kSuspendUserUart = 4U,
    kEnterProgramMode = 5U,
    kSetSambaBaud = 6U,
    kWrite = 7U,
    kBinaryReplyTimeout = 8U,
    kBinaryReplyInvalid = 9U,
    kVersionReplyTimeout = 10U,
    kVersionReplyInvalid = 11U,
    kMissingCapabilities = 12U,
    kAppletSource = 13U,
    kEraseReplyTimeout = 14U,
    kEraseReplyInvalid = 15U,
    kSelectReplyTimeout = 16U,
    kSelectReplyInvalid = 17U,
    kWriteReplyTimeout = 18U,
    kWriteReplyInvalid = 19U,
    kChecksumReplyTimeout = 20U,
    kChecksumReplyInvalid = 21U,
    kChecksumMismatch = 22U,
    kRestoreUserUart = 23U,
    kReleaseBootstrap = 24U,
};

struct Ra4SambaImageSource {
    bool (*open)(void *context, size_t *size);
    bool (*read)(
        void *context,
        size_t offset,
        uint8_t *destination,
        size_t size);
    void (*close)(void *context);
    void *context;
};

struct Ra4SambaProgrammerDiagnostic {
    uint32_t sequence = 0U;
    Ra4SambaProgrammerState state = Ra4SambaProgrammerState::kIdle;
    Ra4SambaProgrammerError error = Ra4SambaProgrammerError::kNone;
    bool active = false;
    bool succeeded = false;
    uint32_t started_at_ms = 0U;
    uint32_t completed_at_ms = 0U;
    uint32_t transmitted_bytes = 0U;
    uint32_t received_bytes = 0U;
    uint32_t image_bytes = 0U;
    uint32_t verified_bytes = 0U;
    uint16_t chunk_index = 0U;
    char version[128] = {};
};

/*
 * Project-owned RA4M1 SAM-BA programmer for the UNO R4 extended bootloader.
 * It accepts only a bounded, already-verified RA4 OTA cache source and uses
 * the bootloader's documented X/Y/Z/K extension sequence. No desktop BOSSA
 * implementation or host-side protocol dependency is linked into the image.
 */
class Ra4SambaProgrammer {
public:
    static constexpr uint32_t kSambaBaud = 230400U;
    static constexpr uint32_t kApplicationFlashBytes = 256U * 1024U;
    static constexpr uint32_t kFlashPageBytes = 256U;
    /*
     * Keep the SAM-BA staging exchange identical to Arduino BOSSA. The UNO
     * R4 extended bootloader acknowledges a 1 KiB Y write but does not
     * reliably answer the following Z checksum command over this UART path;
     * the established BOSSA write path uses a 4 KiB RAM buffer.
     */
    static constexpr uint32_t kStagingBytes = 4096U;
    static constexpr uint32_t kBootloaderSettleMs = 500U;
    static constexpr uint32_t kAutoBaudSettleMs = 10U;
    static constexpr uint32_t kCommandDataSettleMs = 10U;
    /*
     * The extended RA4 bootloader accepts the established BOSSA staging
     * cadence reliably. Do not turn a staging transfer into one large UART
     * burst: each 64-byte packet is completed on the wire before the next.
     */
    static constexpr uint32_t kPayloadCommandSettleMs = 2U;
    static constexpr uint32_t kPayloadInterpacketSettleMs = 5U;
    static constexpr size_t kPayloadPacketBytes = 64U;
    static constexpr uint32_t kReplyTimeoutMs = 1000U;
    static constexpr uint32_t kLongReplyTimeoutMs = 5000U;
    static constexpr uint32_t kApplicationSettleMs = 50U;
    static constexpr uint32_t kDefaultUserUartBaud = 115200U;

    Ra4SambaProgrammer(
        Ra4BootstrapControl &bootstrap,
        const Ra4SambaLink &link,
        const Ra4SambaUserUartHooks &user_uart);

    bool request_program(const Ra4SambaImageSource &source);
    void poll(uint32_t now_ms);
    bool active() const;
    bool busy() const;
    const Ra4SambaProgrammerDiagnostic &diagnostic() const;

private:
    bool write(const uint8_t *data, size_t size);
    bool write_cstr(const char *text);
    bool set_user_uart_baud(uint32_t baud);
    void start(uint32_t now_ms);
    void fail(Ra4SambaProgrammerError error, uint32_t now_ms);
    void finish(bool succeeded, uint32_t now_ms);
    void begin_recovery(uint32_t now_ms);
    void consume_binary_reply(uint32_t now_ms);
    void consume_version_reply(uint32_t now_ms);
    void consume_short_reply(
        uint8_t expected,
        Ra4SambaProgrammerState next,
        Ra4SambaProgrammerError timeout_error,
        Ra4SambaProgrammerError invalid_error,
        uint32_t now_ms);
    void send_version_request(uint32_t now_ms);
    void stage_applet(uint32_t now_ms);
    void send_applet_payload(uint32_t now_ms);
    void send_applet_words(uint32_t now_ms);
    void send_applet_stack(uint32_t now_ms);
    void erase_application(uint32_t now_ms);
    void stage_next_chunk(uint32_t now_ms);
    void send_chunk_payload(uint32_t now_ms);
    void select_staging_buffer(uint32_t now_ms);
    void write_staged_chunk(uint32_t now_ms);
    void complete_staged_chunk(uint32_t now_ms);
    bool prepare_next_chunk(uint32_t now_ms);
    bool has_required_capabilities() const;
    void close_source();

    Ra4BootstrapControl &bootstrap_;
    Ra4SambaLink link_;
    Ra4SambaUserUartHooks user_uart_;
    Ra4SambaImageSource source_ = {};
    Ra4SambaProgrammerDiagnostic diagnostic_ = {};
    uint32_t deadline_ms_ = 0U;
    uint32_t user_uart_baud_ = kDefaultUserUartBaud;
    size_t image_size_ = 0U;
    size_t image_offset_ = 0U;
    size_t chunk_payload_size_ = 0U;
    size_t chunk_program_size_ = 0U;
    size_t chunk_stage_offset_ = 0U;
    uint32_t chunk_flash_address_ = 0U;
    uint8_t binary_reply_index_ = 0U;
    uint8_t binary_reply_noise_count_ = 0U;
    uint8_t short_reply_index_ = 0U;
    uint8_t version_size_ = 0U;
    uint8_t staging_[kStagingBytes] = {};
    bool request_pending_ = false;
    bool bootloader_session_ = false;
    bool recovery_command_sent_ = false;
    bool source_open_ = false;
};

} // namespace bridge

#endif
