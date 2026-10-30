#ifndef UNOR4_BL616CL_RA4_OTA_AT_SERVICE_H
#define UNOR4_BL616CL_RA4_OTA_AT_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "at_transport.h"
#include "ra4_ota_downloader.h"
#include "ra4_ota_service.h"

namespace bridge {

/*
 * AT compatibility boundary for the RA4M1 OTA cache lifecycle. Network
 * download and SAM-BA programming deliberately remain separate services.
 */
class Ra4OtaAtService {
public:
    explicit Ra4OtaAtService(
        Ra4OtaService &ota,
        const Ra4OtaDownloadBackend &download_backend = {});
    ~Ra4OtaAtService();

    bool handles_command(const at::Command &command) const;
    at::CommandResult handle_command(
        const at::Command &command,
        at::AtTransport &transport);
    void poll(uint32_t now_ms);
    Ra4OtaDownloadDiagnostic download_diagnostic() const;

    Ra4OtaService::Error begin_download(
        const char *path,
        bool format_on_fail);
    Ra4OtaService::Error feed_download(const uint8_t *data, size_t size);
    Ra4OtaService::Error finish_download();

private:
    at::CommandResult handle_begin(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_set_ca(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_download(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_download_start(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_download_progress(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_verify(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_reset(
        const at::Command &command,
        at::AtTransport &transport);
    static bool parse_bool(const char *text, bool &value);
    static bool parse_size(const char *text, size_t maximum, size_t &value);
    bool prepare_download(const at::Command &command);
    void complete_synchronous_download();
    static at::CommandResult set_ca_payload(
        void *context,
        const uint8_t *data,
        size_t size);
    static void write_result(
        at::AtTransport &transport,
        const at::Command &command,
        Ra4OtaService::Error result);

    Ra4OtaService &ota_;
    Ra4OtaDownloader downloader_;
    at::AtTransport *synchronous_transport_ = nullptr;
    uint8_t *ca_certificate_ = nullptr;
    size_t ca_certificate_size_ = 0U;
};

} // namespace bridge

#endif
