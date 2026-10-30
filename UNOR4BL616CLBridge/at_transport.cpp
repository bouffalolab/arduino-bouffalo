#include "at_transport.h"

#include <string.h>
#include <stdlib.h>

namespace bridge::at {

namespace {

bool append_character(char *buffer, size_t &size, size_t capacity, char value)
{
    if (size + 1U >= capacity) {
        return false;
    }

    buffer[size++] = value;
    buffer[size] = '\0';
    return true;
}

} // namespace

AtTransport::AtTransport(Config config, Output output, void *output_context)
    : config_(config), output_(output), output_context_(output_context)
{
    if (config_.max_line_bytes >= kTransportLineCapacity) {
        config_.max_line_bytes = kTransportLineCapacity - 1U;
    }
    if (config_.max_raw_payload_bytes > kTransportRawPayloadCapacity) {
        config_.max_raw_payload_bytes = kTransportRawPayloadCapacity;
    }
    if (config_.max_response_bytes > kTransportResponseCapacity) {
        config_.max_response_bytes = kTransportResponseCapacity;
    }
    if (config_.max_response_bytes < sizeof("ERROR\r\n") - 1U) {
        config_.max_response_bytes = sizeof("ERROR\r\n") - 1U;
    }
    line_[0] = '\0';
}

AtTransport::~AtTransport()
{
    release_response_storage();
}

void AtTransport::set_command_handler(CommandHandler handler, void *context)
{
    command_handler_ = handler;
    command_context_ = context;
}

size_t AtTransport::feed(const uint8_t *data, size_t size, uint32_t now_ms)
{
    current_time_ms_ = now_ms;
    if (data == nullptr || response_ready_ || response_deferred_) {
        return 0U;
    }

    size_t offset = 0;
    while (offset < size) {
        if (raw_payload_pending_) {
            if (raw_payload_timed_out(now_ms)) {
                abort_raw_payload();
                if (response_ready_ || response_deferred_) {
                    break;
                }
                continue;
            }

            const size_t remaining = raw_payload_expected_ - raw_payload_size_;
            const size_t available = size - offset;
            const size_t to_copy = remaining < available ? remaining : available;
            memcpy(raw_payload_ + raw_payload_size_, data + offset, to_copy);
            raw_payload_size_ += to_copy;
            offset += to_copy;

            if (raw_payload_size_ == raw_payload_expected_) {
                complete_raw_payload();
                if (response_ready_) {
                    break;
                }
            }
            continue;
        }

        const uint8_t value = data[offset++];
        if (value == '\r') {
            continue;
        }

        if (value == '\n') {
            if (line_overflow_) {
                line_overflow_ = false;
                line_size_ = 0;
                line_[0] = '\0';
                emit_error();
            } else if (line_size_ != 0U) {
                dispatch_line();
                line_size_ = 0;
                line_[0] = '\0';
                if (response_ready_) {
                    break;
                }
            }
            continue;
        }

        if (line_overflow_) {
            continue;
        }

        if (line_size_ >= config_.max_line_bytes || !append_character(line_, line_size_, sizeof(line_), static_cast<char>(value))) {
            line_overflow_ = true;
            line_size_ = 0;
            line_[0] = '\0';
            continue;
        }
    }

    tick(now_ms);
    return offset;
}

void AtTransport::tick(uint32_t now_ms)
{
    current_time_ms_ = now_ms;
    if (raw_payload_pending_ && raw_payload_timed_out(now_ms)) {
        abort_raw_payload();
    }
}

void AtTransport::reset()
{
    release_response_storage();
    line_size_ = 0U;
    line_[0] = '\0';
    line_overflow_ = false;
    dispatching_ = false;
    raw_payload_pending_ = false;
    response_deferred_ = false;
    raw_payload_size_ = 0U;
    raw_payload_expected_ = 0U;
    raw_payload_handler_ = nullptr;
    raw_payload_context_ = nullptr;
    response_size_ = 0U;
    response_position_ = 0U;
    response_building_ = false;
    response_ready_ = false;
    response_overflow_ = false;
}

bool AtTransport::request_raw_payload(size_t size, RawPayloadHandler handler, void *context)
{
    if (!dispatching_ || raw_payload_pending_ || size > config_.max_raw_payload_bytes || handler == nullptr) {
        return false;
    }

    raw_payload_size_ = 0;
    raw_payload_expected_ = size;
    raw_payload_handler_ = handler;
    raw_payload_context_ = context;
    raw_payload_pending_ = true;
    raw_payload_deadline_ms_ = current_time_ms_ + config_.raw_payload_timeout_ms;
    return true;
}

bool AtTransport::raw_payload_pending() const
{
    return raw_payload_pending_;
}

RawPayloadTimeoutDiagnostic AtTransport::raw_payload_timeout_diagnostic() const
{
    return raw_timeout_diagnostic_;
}

bool AtTransport::response_deferred() const
{
    return response_deferred_;
}

bool AtTransport::output_pending() const
{
    return response_ready_;
}

uint8_t *AtTransport::temporary_workspace(size_t size)
{
    if (raw_payload_pending_ ||
        borrowed_response_ != nullptr || response_ready_ ||
        size == 0U ||
        size > config_.max_raw_payload_bytes) {
        return nullptr;
    }
    return raw_payload_;
}

bool AtTransport::complete_deferred_response(CommandResult result)
{
    if (!response_deferred_ || result == CommandResult::Deferred) {
        return false;
    }
    response_deferred_ = false;
    finish(result);
    return true;
}

void AtTransport::flush_output()
{
    if (!response_ready_ || output_ == nullptr) {
        return;
    }

    size_t remaining = response_size_ - response_position_;
    const uint8_t *data = response_storage();
    if (borrowed_response_ != nullptr) {
        if (response_position_ < borrowed_response_offset_) {
            data += response_position_;
            remaining = borrowed_response_offset_ - response_position_;
        } else if (response_position_ < borrowed_response_offset_ + borrowed_response_size_) {
            const size_t position = response_position_ - borrowed_response_offset_;
            data = borrowed_response_ + position;
            remaining = borrowed_response_size_ - position;
        } else {
            data = response_storage() + response_position_ - borrowed_response_size_;
        }
    } else {
        data += response_position_;
    }
    const size_t written = output_(output_context_, data, remaining);
    response_position_ += written <= remaining ? written : remaining;

    if (response_position_ == response_size_) {
        response_size_ = 0U;
        response_position_ = 0U;
        response_ready_ = false;
        release_response_storage();
    }
}

bool AtTransport::begin_response()
{
    if (response_ready_) {
        return false;
    }

    if (!response_building_) {
        release_response_storage();
        response_size_ = 0U;
        response_position_ = 0U;
        response_building_ = true;
        response_overflow_ = false;
    }
    return true;
}

bool AtTransport::append_response(const void *data, size_t size)
{
    if (data == nullptr || size == 0U) {
        return data != nullptr || size == 0U;
    }
    if (response_overflow_ || size > config_.max_response_bytes - response_size_) {
        response_overflow_ = true;
        return false;
    }

    const size_t stored = response_size_ - borrowed_response_size_;
    if (!reserve_response_storage(stored + size)) {
        response_overflow_ = true;
        return false;
    }
    memcpy(response_storage() + stored, data, size);
    response_size_ += size;
    return true;
}

uint8_t *AtTransport::response_storage()
{
    return extended_response_ != nullptr ? extended_response_ : response_;
}

bool AtTransport::reserve_response_storage(size_t size)
{
    if (extended_response_ != nullptr || size <= sizeof(response_)) {
        return true;
    }
    auto *storage = static_cast<uint8_t *>(malloc(config_.max_response_bytes));
    if (storage == nullptr) {
        return false;
    }
    memcpy(storage, response_, response_size_ - borrowed_response_size_);
    extended_response_ = storage;
    return true;
}

void AtTransport::release_response_storage()
{
    if (extended_response_ != nullptr) {
        free(extended_response_);
    }
    extended_response_ = nullptr;
    borrowed_response_ = nullptr;
    borrowed_response_size_ = 0U;
    borrowed_response_offset_ = 0U;
}

void AtTransport::commit_response()
{
    if (!response_building_) {
        return;
    }

    if (response_overflow_) {
        release_response_storage();
        static constexpr char kError[] = "ERROR\r\n";
        memcpy(response_, kError, sizeof(kError) - 1U);
        response_size_ = sizeof(kError) - 1U;
    }

    response_position_ = 0U;
    response_ready_ = response_size_ != 0U;
    response_building_ = false;
    response_overflow_ = false;
}

void AtTransport::emit_error()
{
    if (!begin_response()) {
        return;
    }
    static constexpr char kError[] = "ERROR\r\n";
    (void)append_response(kError, sizeof(kError) - 1U);
    commit_response();
}

bool AtTransport::write_data(const void *data, size_t size)
{
    if (!begin_response()) {
        return false;
    }
    return append_response(data, size);
}

bool AtTransport::write_borrowed_data(const uint8_t *data, size_t size)
{
    if (!begin_response()) {
        return false;
    }
    if (size == 0U) {
        return true;
    }
    if (data == nullptr || response_overflow_ ||
        size > config_.max_response_bytes - response_size_) {
        response_overflow_ = true;
        return false;
    }
    if (borrowed_response_ != nullptr) {
        return append_response(data, size);
    }
    borrowed_response_ = data;
    borrowed_response_size_ = size;
    borrowed_response_offset_ = response_size_;
    response_size_ += size;
    return true;
}

bool AtTransport::write_cstr(const char *text)
{
    if (text == nullptr) {
        return false;
    }
    return write_data(text, strlen(text));
}

bool AtTransport::write_response(const Command &command, const char *text)
{
    return write_cstr(command.name) && write_cstr(": ") && write_cstr(text) && write_cstr("\r\n");
}

bool AtTransport::write_ok()
{
    const bool immediate = !response_building_;
    const bool written = write_cstr("OK\r\n");
    if (immediate) {
        commit_response();
    }
    return written;
}

bool AtTransport::write_error()
{
    const bool immediate = !response_building_;
    const bool written = write_cstr("ERROR\r\n");
    if (immediate) {
        commit_response();
    }
    return written;
}

bool AtTransport::parse_line(const char *line, size_t line_size, Command &command) const
{
    if (line_size < 2U || line[0] != 'A' || line[1] != 'T') {
        return false;
    }

    memset(&command, 0, sizeof(command));
    command.mode = CommandMode::Run;

    size_t separator = 2U;
    while (separator < line_size && line[separator] != '?' && line[separator] != '=') {
        ++separator;
    }

    const size_t name_size = separator - 2U;
    if (name_size >= sizeof(command.name)) {
        return false;
    }
    memcpy(command.name, line + 2U, name_size);
    command.name[name_size] = '\0';

    if (separator == line_size) {
        return true;
    }

    if (line[separator] == '?') {
        if (separator + 1U != line_size) {
            return false;
        }
        command.mode = CommandMode::Read;
        return true;
    }

    if (separator + 2U == line_size && line[separator + 1U] == '?') {
        command.mode = CommandMode::Test;
        return true;
    }

    command.mode = CommandMode::Write;
    return parse_arguments(line + separator + 1U, line_size - separator - 1U, command);
}

bool AtTransport::parse_arguments(const char *text, size_t text_size, Command &command) const
{
    /*
     * WiFiS3 uses a trailing '=' for parameterless write commands such as
     * AT+GETTIME=. Treat that form as an empty argument list, while keeping
     * explicit empty comma-delimited fields observable to their handlers.
     */
    if (text_size == 0U) {
        command.argument_count = 0U;
        return true;
    }

    command.argument_count = 1U;
    size_t argument_size = 0;
    bool quoted = false;
    bool escaped = false;

    for (size_t index = 0; index < text_size; ++index) {
        const char value = text[index];
        char *argument = command.arguments[command.argument_count - 1U];

        if (escaped) {
            if (!append_character(argument, argument_size, kCommandArgumentBytes, value)) {
                return false;
            }
            escaped = false;
        } else if (value == '\\') {
            escaped = true;
        } else if (value == '"') {
            quoted = !quoted;
        } else if (value == ',' && !quoted) {
            if (command.argument_count == kCommandArgumentCapacity) {
                return false;
            }
            ++command.argument_count;
            argument_size = 0;
            command.arguments[command.argument_count - 1U][0] = '\0';
        } else if (!append_character(argument, argument_size, kCommandArgumentBytes, value)) {
            return false;
        }
    }

    if (escaped && !append_character(command.arguments[command.argument_count - 1U], argument_size, kCommandArgumentBytes, '\\')) {
        return false;
    }
    return !quoted;
}

void AtTransport::dispatch_line()
{
    Command command = {};
    if (!parse_line(line_, line_size_, command)) {
        emit_error();
        return;
    }

    if (command.name[0] == '\0') {
        (void)write_ok();
        return;
    }

    if (command_handler_ == nullptr) {
        emit_error();
        return;
    }

    if (!begin_response()) {
        return;
    }
    dispatching_ = true;
    const CommandResult result = command_handler_(command_context_, command, *this);
    dispatching_ = false;

    if (result == CommandResult::Deferred) {
        if (raw_payload_pending_) {
            return;
        }
        response_size_ = 0U;
        response_position_ = 0U;
        response_building_ = false;
        response_overflow_ = false;
        release_response_storage();
        response_deferred_ = true;
        return;
    }

    if (raw_payload_pending_) {
        raw_payload_size_ = 0;
        raw_payload_expected_ = 0;
        raw_payload_handler_ = nullptr;
        raw_payload_context_ = nullptr;
        raw_payload_pending_ = false;
        response_overflow_ = true;
        commit_response();
        return;
    }

    finish(result);
}

void AtTransport::complete_raw_payload()
{
    RawPayloadHandler handler = raw_payload_handler_;
    void *context = raw_payload_context_;
    raw_payload_pending_ = false;
    raw_payload_expected_ = 0;
    raw_payload_handler_ = nullptr;
    raw_payload_context_ = nullptr;

    dispatching_ = true;
    const CommandResult result = handler(context, raw_payload_, raw_payload_size_);
    dispatching_ = false;
    raw_payload_size_ = 0;
    finish(result);
}

void AtTransport::abort_raw_payload()
{
    ++raw_timeout_diagnostic_.count;
    raw_timeout_diagnostic_.expected = static_cast<uint32_t>(raw_payload_expected_);
    raw_timeout_diagnostic_.received = static_cast<uint32_t>(raw_payload_size_);
    raw_payload_pending_ = false;
    raw_payload_size_ = 0;
    raw_payload_expected_ = 0;
    raw_payload_handler_ = nullptr;
    raw_payload_context_ = nullptr;
    response_overflow_ = true;
    commit_response();
}

bool AtTransport::raw_payload_timed_out(uint32_t now_ms) const
{
    return static_cast<int32_t>(now_ms - raw_payload_deadline_ms_) >= 0;
}

void AtTransport::finish(CommandResult result)
{
    if (result == CommandResult::Ok) {
        (void)write_cstr("OK\r\n");
    } else if (result == CommandResult::Error) {
        (void)write_cstr("ERROR\r\n");
    }
    commit_response();
}

} // namespace bridge::at
