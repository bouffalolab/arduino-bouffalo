#include "ra4_ota_downloader.h"

#include "tcp_client_service.h"
#include "tls_client_service.h"

namespace bridge {

namespace {

struct DownloadConnection {
    bool open = false;
    bool secure = false;
    int handle = -1;
    Ra4OtaDownloadBackendDiagnostic diagnostic;
};

DownloadConnection connection;

bool open_connection(
    void *,
    bool secure,
    const char *hostname,
    uint16_t port,
    const uint8_t *ca_certificate,
    size_t ca_certificate_size,
    uint32_t timeout_ms,
    int *handle)
{
    if (connection.open || hostname == nullptr || hostname[0] == '\0' ||
        port == 0U || timeout_ms == 0U || handle == nullptr) {
        return false;
    }
    connection.diagnostic = {};

    int backend_handle = -1;
    if (secure) {
        const TlsClientBackend &backend = bouffalo_tls_client_backend();
        if (ca_certificate == nullptr || ca_certificate_size == 0U ||
            backend.create == nullptr || backend.connect_hostname == nullptr ||
            !backend.create(backend.context, &backend_handle) ||
            !backend.connect_hostname(
                backend.context,
                backend_handle,
                hostname,
                port,
                timeout_ms,
                ca_certificate,
                ca_certificate_size,
                nullptr,
                0U,
                nullptr,
                0U)) {
            if (backend_handle >= 0 && backend.close != nullptr) {
                (void)backend.close(backend.context, backend_handle);
            }
            return false;
        }
    } else {
        const TcpClientBackend &backend = bouffalo_tcp_client_backend();
        uint8_t address[4] = {};
        if (backend.create == nullptr || backend.resolve_hostname == nullptr ||
            backend.connect == nullptr ||
            !backend.create(backend.context, &backend_handle) ||
            !backend.resolve_hostname(backend.context, hostname, address) ||
            !backend.connect(
                backend.context,
                backend_handle,
                address,
                port,
                timeout_ms)) {
            if (backend_handle >= 0 && backend.close != nullptr) {
                (void)backend.close(backend.context, backend_handle);
            }
            return false;
        }
    }

    connection.open = true;
    connection.secure = secure;
    connection.handle = backend_handle;
    connection.diagnostic.open = true;
    connection.diagnostic.secure = secure;
    connection.diagnostic.handle = backend_handle;
    *handle = backend_handle;
    return true;
}

bool write_connection(
    void *,
    int handle,
    const uint8_t *data,
    size_t size)
{
    if (!connection.open || connection.handle != handle || data == nullptr ||
        size == 0U) {
        return false;
    }
    size_t sent = 0U;
    if (connection.secure) {
        const TlsClientBackend &backend = bouffalo_tls_client_backend();
        return backend.send != nullptr &&
            backend.send(backend.context, handle, data, size, &sent) && sent == size;
    }
    const TcpClientBackend &backend = bouffalo_tcp_client_backend();
    return backend.send != nullptr &&
        backend.send(backend.context, handle, data, size, &sent) && sent == size;
}

Ra4OtaDownloadReadResult read_connection(
    void *,
    int handle,
    uint8_t *data,
    size_t capacity,
    size_t *size)
{
    if (!connection.open || connection.handle != handle || data == nullptr ||
        capacity == 0U || size == nullptr) {
        return Ra4OtaDownloadReadResult::Error;
    }
    *size = 0U;
    size_t available = 0U;
    bool ready = false;
    bool received = false;
    ++connection.diagnostic.available_call_count;
    if (connection.secure) {
        const TlsClientBackend &backend = bouffalo_tls_client_backend();
        ready = backend.available != nullptr &&
            backend.available(backend.context, handle, &available);
        if (ready && available != 0U) {
            received = backend.receive != nullptr &&
                backend.receive(backend.context, handle, data, capacity, size);
        }
    } else {
        const TcpClientBackend &backend = bouffalo_tcp_client_backend();
        ready = backend.available != nullptr &&
            backend.available(backend.context, handle, &available);
        if (ready && available != 0U) {
            received = backend.receive != nullptr &&
                backend.receive(backend.context, handle, data, capacity, size);
        }
    }
    connection.diagnostic.last_available = available;
    if (!ready) {
        ++connection.diagnostic.available_failure_count;
    } else if (available == 0U) {
        ++connection.diagnostic.available_pending_count;
    } else {
        ++connection.diagnostic.available_ready_count;
        ++connection.diagnostic.receive_call_count;
        connection.diagnostic.last_receive_succeeded = received;
        connection.diagnostic.last_receive_size = *size;
        if (!received) {
            ++connection.diagnostic.receive_failure_count;
        }
    }
    if (!ready || !received && available != 0U || *size > capacity) {
        return Ra4OtaDownloadReadResult::Error;
    }
    return *size == 0U
        ? Ra4OtaDownloadReadResult::Pending
        : Ra4OtaDownloadReadResult::Data;
}

void close_connection(void *, int handle)
{
    if (!connection.open || connection.handle != handle) {
        return;
    }
    if (connection.secure) {
        const TlsClientBackend &backend = bouffalo_tls_client_backend();
        if (backend.close != nullptr) {
            (void)backend.close(backend.context, handle);
        }
    } else {
        const TcpClientBackend &backend = bouffalo_tcp_client_backend();
        if (backend.close != nullptr) {
            (void)backend.close(backend.context, handle);
        }
    }
    connection.open = false;
    connection.secure = false;
    connection.handle = -1;
    connection.diagnostic.open = false;
    connection.diagnostic.secure = false;
    connection.diagnostic.handle = -1;
}

} // namespace

const Ra4OtaDownloadBackend &bouffalo_ra4_ota_download_backend()
{
    static const Ra4OtaDownloadBackend backend = {
        nullptr,
        open_connection,
        write_connection,
        read_connection,
        close_connection,
    };
    return backend;
}

Ra4OtaDownloadBackendDiagnostic
bouffalo_ra4_ota_download_backend_diagnostic_snapshot()
{
    return connection.diagnostic;
}

} // namespace bridge
