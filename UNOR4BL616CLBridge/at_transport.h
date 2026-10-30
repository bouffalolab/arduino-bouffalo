#ifndef UNOR4_BL616CL_AT_TRANSPORT_H
#define UNOR4_BL616CL_AT_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

namespace bridge::at {

constexpr size_t kCommandNameCapacity = 96U;
constexpr size_t kCommandArgumentCapacity = 12U;
constexpr size_t kCommandArgumentBytes = 256U;
constexpr size_t kTransportLineCapacity = 1024U;
constexpr size_t kTransportRawPayloadCapacity = 16384U;
constexpr size_t kTransportResponseCapacity = kTransportRawPayloadCapacity + kTransportLineCapacity + 16U;
constexpr size_t kTransportInlineResponseCapacity = 4096U;

enum class CommandMode : uint8_t {
    Run,
    Write,
    Read,
    Test,
};

enum class CommandResult : uint8_t {
    Ok,
    Error,
    Deferred,
    Custom,
};

struct Command {
    CommandMode mode;
    char name[kCommandNameCapacity];
    char arguments[kCommandArgumentCapacity][kCommandArgumentBytes];
    size_t argument_count;
};

struct RawPayloadTimeoutDiagnostic {
    uint32_t count = 0U;
    uint32_t expected = 0U;
    uint32_t received = 0U;
};

class AtTransport {
public:
    struct Config {
        size_t max_line_bytes;
        size_t max_raw_payload_bytes;
        size_t max_response_bytes;
        uint32_t raw_payload_timeout_ms;
    };

    typedef size_t (*Output)(void *context, const uint8_t *data, size_t size);
    typedef CommandResult (*RawPayloadHandler)(void *context, const uint8_t *data, size_t size);
    typedef CommandResult (*CommandHandler)(void *context, const Command &command, AtTransport &transport);

    AtTransport(Config config, Output output, void *output_context);
    ~AtTransport();
    AtTransport(const AtTransport &) = delete;
    AtTransport &operator=(const AtTransport &) = delete;

    void set_command_handler(CommandHandler handler, void *context);
    size_t feed(const uint8_t *data, size_t size, uint32_t now_ms);
    void tick(uint32_t now_ms);
    void reset();

    bool request_raw_payload(size_t size, RawPayloadHandler handler, void *context);
    bool raw_payload_pending() const;
    RawPayloadTimeoutDiagnostic raw_payload_timeout_diagnostic() const;
    bool response_deferred() const;
    bool output_pending() const;
    /*
     * Returns the transport-owned raw-payload workspace when it is idle.
     * Callers copy bytes into the response or submit them with
     * write_borrowed_data() before returning from the command handler.
     * A borrowed response prevents workspace reuse until output completes.
     */
    uint8_t *temporary_workspace(size_t size);
    void flush_output();
    bool complete_deferred_response(CommandResult result);

    bool write_data(const void *data, size_t size);
    // The caller keeps these bytes valid until output_pending() becomes false
    // or reset() cancels the response. Input stays paused during that interval.
    bool write_borrowed_data(const uint8_t *data, size_t size);
    bool write_cstr(const char *text);
    bool write_response(const Command &command, const char *text);
    bool write_ok();
    bool write_error();

private:
    bool begin_response();
    bool append_response(const void *data, size_t size);
    bool reserve_response_storage(size_t size);
    uint8_t *response_storage();
    void release_response_storage();
    void commit_response();
    void emit_error();
    bool parse_line(const char *line, size_t line_size, Command &command) const;
    bool parse_arguments(const char *text, size_t text_size, Command &command) const;
    void dispatch_line();
    void complete_raw_payload();
    void abort_raw_payload();
    bool raw_payload_timed_out(uint32_t now_ms) const;
    void finish(CommandResult result);

    Config config_;
    Output output_;
    void *output_context_;
    CommandHandler command_handler_ = nullptr;
    void *command_context_ = nullptr;
    char line_[kTransportLineCapacity];
    size_t line_size_ = 0;
    bool line_overflow_ = false;
    bool dispatching_ = false;
    uint8_t response_[kTransportInlineResponseCapacity];
    uint8_t *extended_response_ = nullptr;
    const uint8_t *borrowed_response_ = nullptr;
    size_t borrowed_response_size_ = 0U;
    size_t borrowed_response_offset_ = 0U;
    size_t response_size_ = 0U;
    size_t response_position_ = 0U;
    bool response_building_ = false;
    bool response_ready_ = false;
    bool response_overflow_ = false;

    RawPayloadTimeoutDiagnostic raw_timeout_diagnostic_;
    bool raw_payload_pending_ = false;
    bool response_deferred_ = false;
    uint32_t current_time_ms_ = 0;
    uint32_t raw_payload_deadline_ms_ = 0;
    uint8_t raw_payload_[kTransportRawPayloadCapacity];
    size_t raw_payload_size_ = 0;
    size_t raw_payload_expected_ = 0;
    RawPayloadHandler raw_payload_handler_ = nullptr;
    void *raw_payload_context_ = nullptr;
};

} // namespace bridge::at

#endif
