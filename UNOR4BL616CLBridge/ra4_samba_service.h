#ifndef UNOR4_BL616CL_RA4_SAMBA_SERVICE_H
#define UNOR4_BL616CL_RA4_SAMBA_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "ra4_bootstrap_control.h"

namespace bridge {

enum class Ra4SambaState : uint8_t {
    kIdle = 0U,
    kWaitBootloader = 1U,
    kWaitAutoBaudSecond = 2U,
    kWaitAutoBaudConfirm = 3U,
    kWaitBinaryReply = 4U,
    kWaitVersionReply = 5U,
    kWaitApplication = 6U,
    kRecovery = 7U,
};

enum class Ra4SambaError : uint8_t {
    kNone = 0U,
    kBusy = 1U,
    kSuspendUserUart = 2U,
    kEnterProgramMode = 3U,
    kSetSambaBaud = 4U,
    kWrite = 5U,
    kBinaryReplyTimeout = 6U,
    kBinaryReplyInvalid = 7U,
    kVersionReplyTimeout = 8U,
    kVersionReplyInvalid = 9U,
    kRestoreUserUart = 10U,
    kReleaseBootstrap = 11U,
};

struct Ra4SambaDiagnostic {
    uint32_t sequence = 0U;
    Ra4SambaState state = Ra4SambaState::kIdle;
    Ra4SambaError error = Ra4SambaError::kNone;
    bool active = false;
    bool succeeded = false;
    uint32_t started_at_ms = 0U;
    uint32_t completed_at_ms = 0U;
    uint32_t transmitted_bytes = 0U;
    uint32_t received_bytes = 0U;
    uint8_t binary_reply_bytes[2] = {};
    char version[128] = {};
};

struct Ra4SambaLink {
    bool (*update_baud)(void *context, uint32_t baud);
    void (*clear_rx)(void *context);
    size_t (*write)(void *context, const uint8_t *data, size_t size);
    int (*read)(void *context);
    void *context;
};

struct Ra4SambaUserUartHooks {
    bool (*suspend_bridge)(void *context);
    bool (*resume_bridge)(void *context);
    uint32_t (*current_baud)(void *context);
    void *context;
};

/*
 * Minimal project-owned SAM-BA transport lifecycle. This first stage only
 * proves that the RA4M1 bootloader can be entered, queried, returned to its
 * application, and that the USB CDC user-UART bridge recovers cleanly.
 * It intentionally contains no SAM-BA write, erase, or Flash command.
 */
class Ra4SambaService {
public:
    static constexpr uint32_t kSambaBaud = 230400U;
    static constexpr uint32_t kBootloaderSettleMs = 500U;
    static constexpr uint32_t kAutoBaudSettleMs = 10U;
    static constexpr uint32_t kReplyTimeoutMs = 1000U;
    static constexpr uint32_t kApplicationSettleMs = 50U;
    static constexpr uint32_t kDefaultUserUartBaud = 115200U;

    Ra4SambaService(
        Ra4BootstrapControl &bootstrap,
        const Ra4SambaLink &link,
        const Ra4SambaUserUartHooks &user_uart);

    bool request_read_only_handshake();
    void poll(uint32_t now_ms);
    bool active() const;
    bool busy() const;
    const Ra4SambaDiagnostic &diagnostic() const;

private:
    bool write(const uint8_t *data, size_t size);
    bool set_user_uart_baud(uint32_t baud);
    void start(uint32_t now_ms);
    void fail(Ra4SambaError error, uint32_t now_ms);
    void finish(bool succeeded, uint32_t now_ms);
    void begin_recovery(uint32_t now_ms);
    void consume_binary_reply(uint32_t now_ms);
    void consume_version_reply(uint32_t now_ms);
    void send_version_request(uint32_t now_ms);

    Ra4BootstrapControl &bootstrap_;
    Ra4SambaLink link_;
    Ra4SambaUserUartHooks user_uart_;
    Ra4SambaDiagnostic diagnostic_ = {};
    uint32_t deadline_ms_ = 0U;
    uint32_t user_uart_baud_ = kDefaultUserUartBaud;
    uint8_t binary_reply_index_ = 0U;
    uint8_t binary_reply_capture_size_ = 0U;
    uint8_t binary_reply_noise_count_ = 0U;
    uint8_t version_size_ = 0U;
    bool request_pending_ = false;
    bool recovery_command_sent_ = false;
    bool bootloader_session_ = false;
};

} // namespace bridge

#endif
