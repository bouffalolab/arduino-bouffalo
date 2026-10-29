#include "ra4_ota_at_service.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ARDUINO)
#include "Arduino.h"
#endif

namespace bridge {

namespace {

constexpr char kOtaBeginCommand[] = "+OTABEGIN";
constexpr char kOtaSetCaRootCommand[] = "+OTASETCAROOT";
constexpr char kOtaDownloadCommand[] = "+OTADOWNLOAD";
constexpr char kOtaDownloadStartCommand[] = "+OTADOWNLOADSTART";
constexpr char kOtaDownloadProgressCommand[] = "+OTADOWNLOADPROGRESS";
constexpr char kOtaVerifyCommand[] = "+OTAVERIFY";
constexpr char kOtaResetCommand[] = "+OTARESET";

uint32_t current_time_ms()
{
#if defined(ARDUINO)
    return millis();
#else
    return 0U;
#endif
}

bool is_name(const at::Command &command, const char *name)
{
    return strcmp(command.name, name) == 0;
}

} // namespace

Ra4OtaAtService::Ra4OtaAtService(
    Ra4OtaService &ota,
    const Ra4OtaDownloadBackend &download_backend)
    : ota_(ota),
      downloader_(ota, download_backend)
{
}

Ra4OtaAtService::~Ra4OtaAtService()
{
    downloader_.cancel();
    free(ca_certificate_);
}

bool Ra4OtaAtService::handles_command(const at::Command &command) const
{
    return is_name(command, kOtaBeginCommand) ||
        is_name(command, kOtaSetCaRootCommand) ||
        is_name(command, kOtaDownloadCommand) ||
        is_name(command, kOtaDownloadStartCommand) ||
        is_name(command, kOtaDownloadProgressCommand) ||
        is_name(command, kOtaVerifyCommand) ||
        is_name(command, kOtaResetCommand);
}

at::CommandResult Ra4OtaAtService::handle_command(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (is_name(command, kOtaBeginCommand)) {
        return handle_begin(command, transport);
    }
    if (is_name(command, kOtaSetCaRootCommand)) {
        return handle_set_ca(command, transport);
    }
    if (is_name(command, kOtaDownloadCommand)) {
        return handle_download(command, transport);
    }
    if (is_name(command, kOtaDownloadStartCommand)) {
        return handle_download_start(command, transport);
    }
    if (is_name(command, kOtaDownloadProgressCommand)) {
        return handle_download_progress(command, transport);
    }
    if (is_name(command, kOtaVerifyCommand)) {
        return handle_verify(command, transport);
    }
    if (is_name(command, kOtaResetCommand)) {
        return handle_reset(command, transport);
    }
    return at::CommandResult::Error;
}

void Ra4OtaAtService::poll(uint32_t now_ms)
{
    downloader_.poll(now_ms);
    complete_synchronous_download();
}

Ra4OtaDownloadDiagnostic Ra4OtaAtService::download_diagnostic() const
{
    return downloader_.diagnostic();
}

Ra4OtaService::Error Ra4OtaAtService::begin_download(
    const char *path,
    bool format_on_fail)
{
    return ota_.begin(path, format_on_fail);
}

Ra4OtaService::Error Ra4OtaAtService::feed_download(
    const uint8_t *data,
    size_t size)
{
    return ota_.feed(data, size);
}

Ra4OtaService::Error Ra4OtaAtService::finish_download()
{
    return ota_.finish();
}

at::CommandResult Ra4OtaAtService::handle_begin(
    const at::Command &command,
    at::AtTransport &transport)
{
    /*
     * The ESP32-S3 run form starts ESP self OTA. BL616CL self FOTA is a
     * distinct feature and must not be mistaken for the RA4M1 cache flow.
     */
    if (command.mode != at::CommandMode::Write ||
        (command.argument_count != 1U && command.argument_count != 2U) ||
        command.arguments[0][0] == '\0') {
        return at::CommandResult::Error;
    }

    bool format_on_fail = true;
    if (command.argument_count == 2U &&
        !parse_bool(command.arguments[1], format_on_fail)) {
        return at::CommandResult::Error;
    }

    const Ra4OtaService::Error result = begin_download(
        command.arguments[0],
        format_on_fail);
    write_result(transport, command, result);
    return at::CommandResult::Ok;
}

at::CommandResult Ra4OtaAtService::handle_set_ca(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U) {
        return at::CommandResult::Error;
    }
    size_t size = 0U;
    if (!parse_size(
            command.arguments[0],
            at::kTransportRawPayloadCapacity,
            size) ||
        size == 0U ||
        !transport.request_raw_payload(
            size,
            &Ra4OtaAtService::set_ca_payload,
            this)) {
        return at::CommandResult::Error;
    }
    return at::CommandResult::Deferred;
}

at::CommandResult Ra4OtaAtService::handle_download_start(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (!prepare_download(command)) {
        return at::CommandResult::Error;
    }

    const Ra4OtaService::Error result = downloader_.start(
        command.arguments[0],
        ca_certificate_,
        ca_certificate_size_,
        current_time_ms());
    write_result(transport, command, result);
    return at::CommandResult::Ok;
}

at::CommandResult Ra4OtaAtService::handle_download(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (!prepare_download(command)) {
        return at::CommandResult::Error;
    }

    const Ra4OtaService::Error result = downloader_.start(
        command.arguments[0],
        ca_certificate_,
        ca_certificate_size_,
        current_time_ms());
    if (result != Ra4OtaService::Error::None) {
        write_result(transport, command, result);
        return at::CommandResult::Ok;
    }
    synchronous_transport_ = &transport;
    return at::CommandResult::Deferred;
}

at::CommandResult Ra4OtaAtService::handle_download_progress(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Run || command.argument_count != 0U) {
        return at::CommandResult::Error;
    }
    if (downloader_.state() == Ra4OtaDownloader::State::Failed) {
        write_result(transport, command, downloader_.error());
        return at::CommandResult::Ok;
    }

    char text[16] = {};
    const int size = snprintf(
        text,
        sizeof(text),
        "%lu",
        static_cast<unsigned long>(downloader_.downloaded_size()));
    if (size <= 0 || static_cast<size_t>(size) >= sizeof(text) ||
        !transport.write_response(command, text)) {
        return at::CommandResult::Error;
    }
    return at::CommandResult::Ok;
}

at::CommandResult Ra4OtaAtService::handle_verify(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Run || command.argument_count != 0U) {
        return at::CommandResult::Error;
    }
    if (downloader_.state() == Ra4OtaDownloader::State::ReceivingHeaders ||
        downloader_.state() == Ra4OtaDownloader::State::ReceivingBody) {
        write_result(transport, command, Ra4OtaService::Error::Download);
        return at::CommandResult::Ok;
    }

    Ra4OtaService::Error result = ota_.error();
    if (result == Ra4OtaService::Error::None &&
        ota_.state() != Ra4OtaService::State::Ready) {
        result = Ra4OtaService::Error::Download;
    }
    write_result(transport, command, result);
    return at::CommandResult::Ok;
}

at::CommandResult Ra4OtaAtService::handle_reset(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Run || command.argument_count != 0U) {
        return at::CommandResult::Error;
    }
    downloader_.cancel();
    ota_.reset();
    write_result(transport, command, Ra4OtaService::Error::None);
    /*
     * This resets only the RA4 OTA cache lifecycle. The ESP32-S3 baseline
     * also used this command to restart its self-OTA partition, but BL616CL
     * self-FOTA has a separate updater path and its external BOOT strap must
     * be controlled by that path before any chip reset is requested.
     */
    return at::CommandResult::Ok;
}

bool Ra4OtaAtService::parse_bool(const char *text, bool &value)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > 1U) {
        return false;
    }
    value = parsed != 0U;
    return true;
}

bool Ra4OtaAtService::parse_size(
    const char *text,
    size_t maximum,
    size_t &value)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > maximum) {
        return false;
    }
    value = static_cast<size_t>(parsed);
    return true;
}

bool Ra4OtaAtService::prepare_download(const at::Command &command)
{
    if (command.mode != at::CommandMode::Write ||
        (command.argument_count != 1U && command.argument_count != 2U) ||
        command.arguments[0][0] == '\0' ||
        synchronous_transport_ != nullptr ||
        downloader_.state() == Ra4OtaDownloader::State::ReceivingHeaders ||
        downloader_.state() == Ra4OtaDownloader::State::ReceivingBody) {
        return false;
    }

    if (command.argument_count == 2U) {
        return command.arguments[1][0] != '\0' &&
            begin_download(command.arguments[1], false) ==
                Ra4OtaService::Error::None;
    }
    return ota_.state() == Ra4OtaService::State::Receiving;
}

void Ra4OtaAtService::complete_synchronous_download()
{
    if (synchronous_transport_ == nullptr) {
        return;
    }
    if (!synchronous_transport_->response_deferred()) {
        downloader_.cancel();
        synchronous_transport_ = nullptr;
        return;
    }
    if (downloader_.state() == Ra4OtaDownloader::State::ReceivingHeaders ||
        downloader_.state() == Ra4OtaDownloader::State::ReceivingBody) {
        return;
    }

    char text[16] = {};
    int size = 0;
    if (downloader_.state() == Ra4OtaDownloader::State::Ready) {
        size = snprintf(
            text,
            sizeof(text),
            "%lu",
            static_cast<unsigned long>(ota_.decoded_size()));
    } else {
        size = snprintf(
            text,
            sizeof(text),
            "%d",
            static_cast<int>(downloader_.error()));
    }
    at::AtTransport *transport = synchronous_transport_;
    synchronous_transport_ = nullptr;
    const bool written = size > 0 && static_cast<size_t>(size) < sizeof(text) &&
        transport->write_cstr(kOtaDownloadCommand) &&
        transport->write_cstr(": ") &&
        transport->write_cstr(text) &&
        transport->write_cstr("\r\n");
    (void)transport->complete_deferred_response(
        written ? at::CommandResult::Ok : at::CommandResult::Error);
}

at::CommandResult Ra4OtaAtService::set_ca_payload(
    void *context,
    const uint8_t *data,
    size_t size)
{
    Ra4OtaAtService *service = static_cast<Ra4OtaAtService *>(context);
    if (data == nullptr || size == 0U) {
        return at::CommandResult::Error;
    }
    /*
     * The native TLS backend passes this buffer to mbedTLS' PEM parser.
     * Keep the raw protocol length exact, but retain a private NUL terminator
     * just as TlsClientService::replace_ca() does.
     */
    uint8_t *copy = static_cast<uint8_t *>(malloc(size + 1U));
    if (copy == nullptr) {
        return at::CommandResult::Error;
    }
    memcpy(copy, data, size);
    copy[size] = '\0';
    free(service->ca_certificate_);
    service->ca_certificate_ = copy;
    service->ca_certificate_size_ = size + 1U;
    return at::CommandResult::Ok;
}

void Ra4OtaAtService::write_result(
    at::AtTransport &transport,
    const at::Command &command,
    Ra4OtaService::Error result)
{
    char text[16] = {};
    const int size = snprintf(
        text,
        sizeof(text),
        "%d",
        static_cast<int>(result));
    if (size > 0 && static_cast<size_t>(size) < sizeof(text)) {
        (void)transport.write_response(command, text);
    }
}

} // namespace bridge
