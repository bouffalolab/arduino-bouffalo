#include "ra4_ota_downloader.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace bridge {

namespace {

bool equal_ignore_case(const uint8_t *left, size_t left_size, const char *right)
{
    const size_t right_size = strlen(right);
    if (left_size != right_size) {
        return false;
    }
    for (size_t index = 0U; index < left_size; ++index) {
        if (tolower(left[index]) != tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

const uint8_t *find_line_end(const uint8_t *data, const uint8_t *end)
{
    for (const uint8_t *cursor = data; cursor + 1U < end; ++cursor) {
        if (cursor[0] == '\r' && cursor[1] == '\n') {
            return cursor;
        }
    }
    return nullptr;
}

const uint8_t *trim_left(const uint8_t *begin, const uint8_t *end)
{
    while (begin < end && (*begin == ' ' || *begin == '\t')) {
        ++begin;
    }
    return begin;
}

const uint8_t *trim_right(const uint8_t *begin, const uint8_t *end)
{
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) {
        --end;
    }
    return end;
}

bool parse_decimal(const uint8_t *begin, const uint8_t *end, size_t *value)
{
    if (begin == nullptr || end == nullptr || value == nullptr || begin == end) {
        return false;
    }
    size_t parsed = 0U;
    for (const uint8_t *cursor = begin; cursor < end; ++cursor) {
        if (*cursor < '0' || *cursor > '9' ||
            parsed > (SIZE_MAX - static_cast<size_t>(*cursor - '0')) / 10U) {
            return false;
        }
        parsed = parsed * 10U + static_cast<size_t>(*cursor - '0');
    }
    *value = parsed;
    return true;
}

bool valid_hostname_character(char value)
{
    return (value >= 'a' && value <= 'z') ||
        (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9') ||
        value == '.' || value == '-';
}

} // namespace

Ra4OtaDownloader::Ra4OtaDownloader(
    Ra4OtaService &ota,
    const Ra4OtaDownloadBackend &backend)
    : ota_(ota),
      backend_(backend)
{
}

Ra4OtaService::Error Ra4OtaDownloader::start(
    const char *url,
    const uint8_t *ca_certificate,
    size_t ca_certificate_size,
    uint32_t now_ms)
{
    cancel();
    secure_ = false;
    last_read_valid_ = false;
    last_read_result_ = Ra4OtaDownloadReadResult::Pending;
    last_read_size_ = 0U;
    timeout_reason_ = Ra4OtaDownloadTimeoutReason::None;
    last_progress_ms_ = now_ms;
    last_poll_ms_ = now_ms;
    poll_count_ = 0U;
    data_count_ = 0U;
    pending_count_ = 0U;
    closed_count_ = 0U;
    error_count_ = 0U;
    if (ota_.state() != Ra4OtaService::State::Receiving) {
        error_ = Ra4OtaService::Error::Download;
        state_ = State::Failed;
        return error_;
    }

    ParsedUrl parsed;
    if (!parse_url(url, parsed)) {
        fail(Ra4OtaService::Error::UrlParse);
        return error_;
    }
    secure_ = parsed.secure;
    if (parsed.secure && (ca_certificate == nullptr || ca_certificate_size == 0U)) {
        fail(Ra4OtaService::Error::ServerConnect);
        return error_;
    }
    if (backend_.open == nullptr || backend_.write == nullptr ||
        backend_.read == nullptr || backend_.close == nullptr ||
        !backend_.open(
            backend_.context,
            parsed.secure,
            parsed.hostname,
            parsed.port,
            ca_certificate,
            ca_certificate_size,
            kHeaderTimeoutMs,
            &handle_)) {
        fail(Ra4OtaService::Error::ServerConnect);
        return error_;
    }
    if (!write_request(parsed)) {
        fail(Ra4OtaService::Error::ServerConnect);
        return error_;
    }

    state_ = State::ReceivingHeaders;
    error_ = Ra4OtaService::Error::None;
    header_size_ = 0U;
    content_length_ = 0U;
    downloaded_size_ = 0U;
    header_error_ = Ra4OtaService::Error::ParseHttpHeader;
    last_progress_ms_ = now_ms;
    return Ra4OtaService::Error::None;
}

void Ra4OtaDownloader::poll(uint32_t now_ms)
{
    if (state_ != State::ReceivingHeaders && state_ != State::ReceivingBody) {
        return;
    }

    ++poll_count_;
    last_poll_ms_ = now_ms;
    size_t read_size = 0U;
    const Ra4OtaDownloadReadResult result = backend_.read(
        backend_.context,
        handle_,
        read_buffer_,
        sizeof(read_buffer_),
        &read_size);
    last_read_valid_ = true;
    last_read_result_ = result;
    last_read_size_ = read_size;
    switch (result) {
    case Ra4OtaDownloadReadResult::Data:
        ++data_count_;
        break;
    case Ra4OtaDownloadReadResult::Pending:
        ++pending_count_;
        break;
    case Ra4OtaDownloadReadResult::Closed:
        ++closed_count_;
        break;
    case Ra4OtaDownloadReadResult::Error:
        ++error_count_;
        break;
    }
    if (result == Ra4OtaDownloadReadResult::Error ||
        read_size > sizeof(read_buffer_)) {
        fail(Ra4OtaService::Error::Download);
        return;
    }
    if (result == Ra4OtaDownloadReadResult::Closed) {
        fail(Ra4OtaService::Error::Download);
        return;
    }
    if (result == Ra4OtaDownloadReadResult::Pending || read_size == 0U) {
        if (timed_out(now_ms)) {
            timeout_reason_ = state_ == State::ReceivingHeaders
                ? Ra4OtaDownloadTimeoutReason::Header
                : Ra4OtaDownloadTimeoutReason::Body;
            fail(state_ == State::ReceivingHeaders
                    ? Ra4OtaService::Error::HeaderTimeout
                    : Ra4OtaService::Error::Download);
        }
        return;
    }

    last_progress_ms_ = now_ms;
    size_t consumed = 0U;
    if (state_ == State::ReceivingHeaders &&
        (!append_header(read_buffer_, read_size, &consumed) ||
         (header_complete() && !parse_response_header()))) {
        fail(header_error_);
        return;
    }
    if (state_ == State::ReceivingHeaders && header_complete()) {
        state_ = State::ReceivingBody;
    }
    if (state_ == State::ReceivingBody &&
        (consumed < read_size && !feed_body(
            read_buffer_ + consumed,
            read_size - consumed))) {
        return;
    }
}

void Ra4OtaDownloader::cancel()
{
    close_connection();
    if (state_ == State::ReceivingHeaders || state_ == State::ReceivingBody) {
        (void)ota_.abort(Ra4OtaService::Error::Download);
    }
    state_ = State::Idle;
    error_ = Ra4OtaService::Error::None;
    header_size_ = 0U;
    content_length_ = 0U;
    downloaded_size_ = 0U;
    header_error_ = Ra4OtaService::Error::ParseHttpHeader;
}

Ra4OtaDownloader::State Ra4OtaDownloader::state() const
{
    return state_;
}

Ra4OtaService::Error Ra4OtaDownloader::error() const
{
    return error_;
}

size_t Ra4OtaDownloader::content_length() const
{
    return content_length_;
}

size_t Ra4OtaDownloader::downloaded_size() const
{
    return downloaded_size_;
}

Ra4OtaDownloadDiagnostic Ra4OtaDownloader::diagnostic() const
{
    Ra4OtaDownloadDiagnostic result;
    result.last_read_result = last_read_result_;
    result.timeout_reason = timeout_reason_;
    result.secure = secure_;
    result.last_read_valid = last_read_valid_;
    result.last_read_size = last_read_size_;
    result.content_length = content_length_;
    result.downloaded_size = downloaded_size_;
    result.poll_count = poll_count_;
    result.data_count = data_count_;
    result.pending_count = pending_count_;
    result.closed_count = closed_count_;
    result.error_count = error_count_;
    result.last_progress_ms = last_progress_ms_;
    result.last_poll_ms = last_poll_ms_;
    return result;
}

bool Ra4OtaDownloader::parse_url(const char *url, ParsedUrl &parsed) const
{
    if (url == nullptr) {
        return false;
    }
    const char *authority = nullptr;
    if (strncmp(url, "http://", 7U) == 0) {
        parsed.secure = false;
        parsed.port = 80U;
        authority = url + 7U;
    } else if (strncmp(url, "https://", 8U) == 0) {
        parsed.secure = true;
        parsed.port = 443U;
        authority = url + 8U;
    } else {
        return false;
    }

    const char *path = strchr(authority, '/');
    const char *authority_end = path != nullptr ? path : authority + strlen(authority);
    if (authority == authority_end || strchr(authority, '@') != nullptr ||
        strchr(authority, '#') != nullptr) {
        return false;
    }
    const char *port_separator = nullptr;
    for (const char *cursor = authority; cursor < authority_end; ++cursor) {
        if (*cursor == ':') {
            if (port_separator != nullptr) {
                return false;
            }
            port_separator = cursor;
        }
    }
    const char *hostname_end =
        port_separator != nullptr ? port_separator : authority_end;
    const size_t hostname_size = static_cast<size_t>(hostname_end - authority);
    if (hostname_size == 0U || hostname_size >= sizeof(parsed.hostname)) {
        return false;
    }
    for (size_t index = 0U; index < hostname_size; ++index) {
        if (!valid_hostname_character(authority[index])) {
            return false;
        }
    }
    memcpy(parsed.hostname, authority, hostname_size);
    parsed.hostname[hostname_size] = '\0';

    if (port_separator != nullptr) {
        size_t port = 0U;
        if (!parse_decimal(
                reinterpret_cast<const uint8_t *>(port_separator + 1U),
                reinterpret_cast<const uint8_t *>(authority_end),
                &port) ||
            port == 0U || port > 65535U) {
            return false;
        }
        parsed.port = static_cast<uint16_t>(port);
    }

    if (path == nullptr) {
        strcpy(parsed.path, "/");
        return true;
    }
    const size_t path_size = strlen(path);
    if (path_size == 0U || path_size >= sizeof(parsed.path) ||
        strchr(path, '#') != nullptr) {
        return false;
    }
    memcpy(parsed.path, path, path_size + 1U);
    return true;
}

bool Ra4OtaDownloader::write_request(const ParsedUrl &url)
{
    char request[kRequestCapacity] = {};
    const bool include_port =
        (url.secure && url.port != 443U) || (!url.secure && url.port != 80U);
    const int size = snprintf(
        request,
        sizeof(request),
        include_port
            ? "GET %s HTTP/1.1\r\nHost: %s:%u\r\n"
              "Accept: application/octet-stream\r\nConnection: close\r\n\r\n"
            : "GET %s HTTP/1.1\r\nHost: %s\r\n"
              "Accept: application/octet-stream\r\nConnection: close\r\n\r\n",
        url.path,
        url.hostname,
        static_cast<unsigned int>(url.port));
    if (size <= 0 || static_cast<size_t>(size) >= sizeof(request)) {
        return false;
    }
    return backend_.write(
        backend_.context,
        handle_,
        reinterpret_cast<const uint8_t *>(request),
        static_cast<size_t>(size));
}

bool Ra4OtaDownloader::append_header(
    const uint8_t *data,
    size_t size,
    size_t *consumed)
{
    if (data == nullptr || consumed == nullptr) {
        return false;
    }
    *consumed = 0U;
    while (*consumed < size && !header_complete()) {
        if (header_size_ == sizeof(header_)) {
            return false;
        }
        header_[header_size_++] = data[(*consumed)++];
    }
    return true;
}

bool Ra4OtaDownloader::parse_response_header()
{
    if (!header_complete()) {
        header_error_ = Ra4OtaService::Error::ParseHttpHeader;
        return false;
    }
    const uint8_t *const begin = header_;
    const uint8_t *const end = header_ + header_size_;
    const uint8_t *line_end = find_line_end(begin, end);
    if (line_end == nullptr || static_cast<size_t>(line_end - begin) < 12U ||
        (memcmp(begin, "HTTP/1.0 ", 9U) != 0 &&
         memcmp(begin, "HTTP/1.1 ", 9U) != 0) ||
        begin[8] != ' ') {
        header_error_ = Ra4OtaService::Error::ParseHttpHeader;
        return false;
    }
    if (begin[9] != '2' || begin[10] != '0' || begin[11] != '0') {
        header_error_ = Ra4OtaService::Error::HttpResponse;
        return false;
    }

    bool found_length = false;
    for (const uint8_t *line = line_end + 2U; line < end; ) {
        line_end = find_line_end(line, end);
        if (line_end == nullptr) {
            header_error_ = Ra4OtaService::Error::ParseHttpHeader;
            return false;
        }
        if (line == line_end) {
            break;
        }
        const uint8_t *separator =
            static_cast<const uint8_t *>(memchr(line, ':', line_end - line));
        if (separator == nullptr) {
            header_error_ = Ra4OtaService::Error::ParseHttpHeader;
            return false;
        }
        const uint8_t *name_end = trim_right(line, separator);
        const uint8_t *value_begin = trim_left(separator + 1U, line_end);
        const uint8_t *value_end = trim_right(value_begin, line_end);
        if (equal_ignore_case(
                line,
                static_cast<size_t>(name_end - line),
                "Content-Length")) {
            if (found_length ||
                !parse_decimal(value_begin, value_end, &content_length_)) {
                header_error_ = Ra4OtaService::Error::ParseHttpHeader;
                return false;
            }
            found_length = true;
        }
        if (equal_ignore_case(
                line,
                static_cast<size_t>(name_end - line),
                "Transfer-Encoding") &&
            !equal_ignore_case(
                value_begin,
                static_cast<size_t>(value_end - value_begin),
                "identity")) {
            header_error_ = Ra4OtaService::Error::HttpHeader;
            return false;
        }
        line = line_end + 2U;
    }
    if (!found_length) {
        header_error_ = Ra4OtaService::Error::HttpHeader;
    }
    return found_length;
}

bool Ra4OtaDownloader::feed_body(const uint8_t *data, size_t size)
{
    if (size > content_length_ - downloaded_size_) {
        fail(Ra4OtaService::Error::Download);
        return false;
    }
    const Ra4OtaService::Error result = ota_.feed(data, size);
    if (result != Ra4OtaService::Error::None) {
        fail(result);
        return false;
    }
    downloaded_size_ += size;
    if (downloaded_size_ != content_length_) {
        return true;
    }
    const Ra4OtaService::Error finished = ota_.finish();
    if (finished != Ra4OtaService::Error::None) {
        fail(finished);
        return false;
    }
    close_connection();
    state_ = State::Ready;
    error_ = Ra4OtaService::Error::None;
    return true;
}

bool Ra4OtaDownloader::header_complete() const
{
    return header_size_ >= 4U &&
        header_[header_size_ - 4U] == '\r' &&
        header_[header_size_ - 3U] == '\n' &&
        header_[header_size_ - 2U] == '\r' &&
        header_[header_size_ - 1U] == '\n';
}

bool Ra4OtaDownloader::timed_out(uint32_t now_ms) const
{
    const uint32_t timeout =
        state_ == State::ReceivingHeaders ? kHeaderTimeoutMs : kBodyTimeoutMs;
    return static_cast<uint32_t>(now_ms - last_progress_ms_) >= timeout;
}

void Ra4OtaDownloader::close_connection()
{
    if (handle_ >= 0 && backend_.close != nullptr) {
        backend_.close(backend_.context, handle_);
    }
    handle_ = -1;
}

void Ra4OtaDownloader::fail(Ra4OtaService::Error error)
{
    close_connection();
    error_ = ota_.abort(error);
    state_ = State::Failed;
}

} // namespace bridge
