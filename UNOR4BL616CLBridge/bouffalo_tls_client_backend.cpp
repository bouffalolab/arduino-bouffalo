#include "tls_client_service.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

extern "C" {
#include "bflb_sec_trng.h"
#include <lwip/api.h>
#include <lwip/errno.h>
#include <lwip/dns.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <lwip/stats.h>
#include <lwip/priv/tcp_priv.h>
#include <lwip/tcpip.h>
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509.h"
#include "mbedtls/x509_crt.h"
#include "sdk/mm/mm.h"
}

namespace bridge {

namespace {

#if !defined(MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH)
#error "The BL616CL bridge TLS profile requires variable mbedTLS buffers"
#endif

constexpr size_t kTlsConnectionCount = kTlsClientSlotCount;
constexpr uint32_t kSocketTimeoutMs = 10000U;
constexpr uint32_t kTlsHandshakeWaitSliceMs = 50U;
#ifndef BL616CL_TLS_BLOCKING_CONNECT_EXPERIMENT
#define BL616CL_TLS_BLOCKING_CONNECT_EXPERIMENT 0
#endif
#ifndef BL616CL_TLS_COALESCE_READ
#define BL616CL_TLS_COALESCE_READ 0
#endif
// Allocate 2 KiB records while processing the certificate handshake, then
// request 1 KiB records so the enabled variable-buffer profile compacts each
// established TLS slot before the next connection verifies its certificate.
constexpr unsigned char kTlsMaxFragmentLength =
    MBEDTLS_SSL_MAX_FRAG_LEN_1024;

struct TlsConnection {
    /*
     * Eight slots remain resident for the bridge lifetime. Keep their state
     * flags packed so allocator diagnostics do not consume the last bytes of
     * the fixed RAM region.
     */
    bool allocated : 1;
    bool connected : 1;
    bool config_initialized : 1;
    bool ssl_initialized : 1;
    bool ca_initialized : 1;
    bool client_certificate_initialized : 1;
    bool client_private_key_initialized : 1;
    bool has_peeked : 1;
    bool resolved_address_valid : 1;
    bool peer_address_valid : 1;
    int socket = -1;
    mbedtls_ssl_config config;
    mbedtls_ssl_context ssl;
    mbedtls_x509_crt ca;
    mbedtls_x509_crt client_certificate;
    mbedtls_pk_context client_private_key;
    uint8_t peeked = 0U;
    uint8_t diagnostic_phase = 0U;
    int32_t diagnostic_error = 0;
    uint32_t verify_flags = 0U;
    uint8_t resolved_address[4] = {};
    uint8_t peer_address[4] = {};
    uint16_t peer_port = 0U;
    uint8_t heap_before_handshake_units = 0U;
    uint8_t heap_failure_units = 0U;
    uint8_t heap_after_cleanup_units = 0U;
    uint8_t ocram_before_setup_units = 0U;
    uint8_t wram_before_setup_units = 0U;
    uint8_t ocram_failure_units = 0U;
    uint8_t wram_failure_units = 0U;
    uint8_t ocram_after_cleanup_units = 0U;
    uint8_t wram_after_cleanup_units = 0U;
    uint8_t wram_max_before_setup_units = 0U;
    uint8_t wram_max_failure_units = 0U;
    uint8_t wram_max_after_cleanup_units = 0U;
    uint16_t connection_timeout_units = 0U;
    uint16_t handshake_elapsed_ms = 0U;
    uint8_t failure_tcp_pcb_used = 0U;
    uint8_t failure_tcp_seg_used = 0U;
    uint8_t failure_netconn_used = 0U;
    uint16_t failure_lwip_heap_used = 0U;
};

TlsConnection connections[kTlsConnectionCount] = {};

TlsConnection *get_connection(int handle)
{
    if (handle < 0 ||
        static_cast<size_t>(handle) >= kTlsConnectionCount ||
        !connections[handle].allocated) {
        return nullptr;
    }
    return &connections[handle];
}

int trng_random(void *, unsigned char *output, size_t size)
{
    return bflb_trng_readlen(output, static_cast<uint32_t>(size)) == 0
        ? 0
        : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

int ssl_send(void *context, const unsigned char *data, size_t size)
{
    TlsConnection *connection = static_cast<TlsConnection *>(context);
    if (connection == nullptr || connection->socket < 0) {
        return MBEDTLS_ERR_NET_INVALID_CONTEXT;
    }
    const int result = send(connection->socket, data, size, 0);
    if (result >= 0) {
        return result;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    if (errno == EPIPE || errno == ECONNRESET) {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

int ssl_recv(void *context, unsigned char *data, size_t size)
{
    TlsConnection *connection = static_cast<TlsConnection *>(context);
    if (connection == nullptr || connection->socket < 0 ||
        data == nullptr || size == 0U) {
        return MBEDTLS_ERR_NET_INVALID_CONTEXT;
    }
    connection->diagnostic_phase = 41U;
    fd_set readable = {};
    FD_SET(connection->socket, &readable);
    struct timeval timeout = {};
    const int selected = select(
        connection->socket + 1,
        &readable,
        nullptr,
        nullptr,
        &timeout);
    if (selected == 0) {
        connection->diagnostic_phase = 42U;
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    if (selected < 0) {
        return errno == EINTR
            ? MBEDTLS_ERR_SSL_WANT_READ
            : MBEDTLS_ERR_NET_RECV_FAILED;
    }
    connection->diagnostic_phase = 43U;
    const int result = recv(connection->socket, data, size, 0);
    if (result >= 0) {
        return result;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    if (errno == EPIPE || errno == ECONNRESET) {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

/*
 * These snapshots are failure diagnostics rather than an allocation profiler.
 * A 1 KiB unit keeps the eight TLS slot records compact while preserving
 * enough resolution to distinguish the multi-kilobyte TLS handshake peaks.
 */
constexpr size_t kHeapDiagnosticUnitBytes = 1024U;

uint8_t heap_free_units(uint32_t heap_id)
{
    const size_t free_bytes = kfree_size(heap_id);
    const size_t units =
        (free_bytes + kHeapDiagnosticUnitBytes - 1U) /
        kHeapDiagnosticUnitBytes;
    if (units > UINT8_MAX) {
        return UINT8_MAX;
    }
    return static_cast<uint8_t>(units);
}

uint8_t wram_max_free_block_units()
{
    mm_heap_t *heap = g_mem_manager.heaps[MM_HEAP_WRAM_0];
    if (heap == nullptr || !heap->is_active ||
        g_mem_manager.allocators[heap->allocator_id] == nullptr ||
        g_mem_manager.allocators[heap->allocator_id]->get_usage_info == nullptr) {
        return 0U;
    }
    mm_usage_info info = {};
    g_mem_manager.allocators[heap->allocator_id]->get_usage_info(heap, &info);
    const size_t units =
        (static_cast<size_t>(info.max_free_size) + kHeapDiagnosticUnitBytes - 1U) /
        kHeapDiagnosticUnitBytes;
    return static_cast<uint8_t>(units > UINT8_MAX ? UINT8_MAX : units);
}

void record_heap_before_setup(TlsConnection &connection)
{
    connection.ocram_before_setup_units =
        heap_free_units(MM_HEAP_OCRAM_0);
    connection.wram_before_setup_units =
        heap_free_units(MM_HEAP_WRAM_0);
    connection.wram_max_before_setup_units = wram_max_free_block_units();
}

void record_heap_before_handshake(TlsConnection &connection)
{
    connection.heap_before_handshake_units = heap_free_units(0U);
}

void record_heap_failure(TlsConnection &connection)
{
    connection.heap_failure_units = heap_free_units(0U);
    connection.ocram_failure_units = heap_free_units(MM_HEAP_OCRAM_0);
    connection.wram_failure_units = heap_free_units(MM_HEAP_WRAM_0);
    connection.wram_max_failure_units = wram_max_free_block_units();
#if MEMP_STATS
    connection.failure_tcp_pcb_used = static_cast<uint8_t>(
        lwip_stats.memp[MEMP_TCP_PCB]->used);
    connection.failure_tcp_seg_used = static_cast<uint8_t>(
        lwip_stats.memp[MEMP_TCP_SEG]->used);
    connection.failure_netconn_used = static_cast<uint8_t>(
        lwip_stats.memp[MEMP_NETCONN]->used);
#endif
#if MEM_STATS
    connection.failure_lwip_heap_used =
        static_cast<uint16_t>(lwip_stats.mem.used);
#endif
}

void record_heap_after_cleanup(TlsConnection &connection)
{
    connection.heap_after_cleanup_units = heap_free_units(0U);
    connection.ocram_after_cleanup_units =
        heap_free_units(MM_HEAP_OCRAM_0);
    connection.wram_after_cleanup_units =
        heap_free_units(MM_HEAP_WRAM_0);
    connection.wram_max_after_cleanup_units = wram_max_free_block_units();
}

void record_handshake_elapsed(
    TlsConnection &connection,
    unsigned long started_at)
{
    const uint32_t elapsed_ms =
        static_cast<uint32_t>(millis() - started_at);
    connection.handshake_elapsed_ms =
        elapsed_ms > UINT16_MAX
        ? UINT16_MAX
        : static_cast<uint16_t>(elapsed_ms);
}

bool set_socket_timeouts(int socket, uint32_t timeout_ms)
{
    struct timeval timeout = {
        static_cast<long>(timeout_ms / 1000U),
        static_cast<long>((timeout_ms % 1000U) * 1000U),
    };
    return setsockopt(
               socket,
               SOL_SOCKET,
               SO_RCVTIMEO,
               &timeout,
               sizeof(timeout)) == 0 &&
        setsockopt(
               socket,
               SOL_SOCKET,
               SO_SNDTIMEO,
               &timeout,
               sizeof(timeout)) == 0;
}

bool close_failed_tcp_socket(int socket, int error)
{
    (void)closesocket(socket);
    errno = error;
    return false;
}

bool open_tcp_socket(
    const uint8_t address[4],
    uint16_t port,
    uint32_t timeout_ms,
    int *socket_out,
    uint8_t *failure_phase)
{
    if (address == nullptr || port == 0U || timeout_ms == 0U ||
        socket_out == nullptr || failure_phase == nullptr) {
        errno = EINVAL;
        return false;
    }
    *failure_phase = 21U;
    const int socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket < 0) {
        return false;
    }
    struct sockaddr_in remote = {};
    remote.sin_len = sizeof(remote);
    remote.sin_family = AF_INET;
    remote.sin_port = htons(port);
    remote.sin_addr.s_addr = htonl(
        (static_cast<uint32_t>(address[0]) << 24U) |
        (static_cast<uint32_t>(address[1]) << 16U) |
        (static_cast<uint32_t>(address[2]) << 8U) |
        static_cast<uint32_t>(address[3]));
#if BL616CL_TLS_BLOCKING_CONNECT_EXPERIMENT
    /*
     * lwIP's blocking netconn_connect() does not return until the TCP
     * connected/error callback runs. Build this branch only for hardware
     * diagnosis when the nonblocking select/SO_ERROR path reports a peer
     * before the Windows listener observes a SYN.
     */
    *failure_phase = 22U;
    if (!set_socket_timeouts(socket, timeout_ms)) {
        return close_failed_tcp_socket(socket, errno);
    }
    *failure_phase = 23U;
    if (connect(
            socket,
            reinterpret_cast<struct sockaddr *>(&remote),
            sizeof(remote)) != 0) {
        return close_failed_tcp_socket(socket, errno);
    }
    *socket_out = socket;
    return true;
#else
    unsigned long nonblocking = 1U;
    *failure_phase = 22U;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) {
        return close_failed_tcp_socket(socket, errno);
    }
    *failure_phase = 23U;
    const int result = connect(
        socket,
        reinterpret_cast<struct sockaddr *>(&remote),
        sizeof(remote));
    if (result != 0 && errno != EINPROGRESS) {
        return close_failed_tcp_socket(socket, errno);
    }

    /*
     * A zero return from nonblocking lwIP connect only means the request was
     * accepted by the local stack on this bridge profile. Require the same
     * select/SO_ERROR completion check as the ESP32 WiFiClient implementation
     * before mbedTLS is allowed to use the socket.
     */
    fd_set writable = {};
    FD_SET(socket, &writable);
    struct timeval timeout = {
        static_cast<long>(timeout_ms / 1000U),
        static_cast<long>((timeout_ms % 1000U) * 1000U),
    };
    int socket_error = 0;
    socklen_t error_length = sizeof(socket_error);
    *failure_phase = 24U;
    const int selected = select(
        socket + 1,
        nullptr,
        &writable,
        nullptr,
        &timeout);
    if (selected <= 0 || !FD_ISSET(socket, &writable)) {
        return close_failed_tcp_socket(
            socket,
            selected < 0 ? errno : (selected == 0 ? ETIMEDOUT : EIO));
    }
    *failure_phase = 25U;
    if (getsockopt(
            socket,
            SOL_SOCKET,
            SO_ERROR,
            &socket_error,
            &error_length) != 0) {
        return close_failed_tcp_socket(socket, errno);
    }
    if (socket_error != 0) {
        return close_failed_tcp_socket(socket, socket_error);
    }
    /*
     * The BL616CL lwIP connect path can replace the descriptor flags when its
     * TCP completion callback runs. Reassert FIONBIO after select/SO_ERROR so
     * mbedTLS gets WANT_READ/WANT_WRITE instead of a long blocking recv().
     */
    nonblocking = 1U;
    *failure_phase = 26U;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0 ||
        !set_socket_timeouts(socket, kTlsHandshakeWaitSliceMs)) {
        return close_failed_tcp_socket(socket, errno);
    }
    *socket_out = socket;
    return true;
#endif
}

void free_tls_context(TlsConnection &connection)
{
    if (connection.ssl_initialized) {
        mbedtls_ssl_free(&connection.ssl);
    }
    if (connection.config_initialized) {
        mbedtls_ssl_config_free(&connection.config);
    }
    if (connection.ca_initialized) {
        mbedtls_x509_crt_free(&connection.ca);
    }
    if (connection.client_certificate_initialized) {
        mbedtls_x509_crt_free(&connection.client_certificate);
    }
    if (connection.client_private_key_initialized) {
        mbedtls_pk_free(&connection.client_private_key);
    }
    connection.ssl_initialized = false;
    connection.config_initialized = false;
    connection.ca_initialized = false;
    connection.client_certificate_initialized = false;
    connection.client_private_key_initialized = false;
}

void release_handshake_ca(TlsConnection &connection)
{
    if (!connection.ca_initialized) {
        return;
    }
    /*
     * The trust anchor is only needed while verifying the peer handshake.
     * Renegotiation is disabled by the mbedTLS client defaults, so detach the
     * config pointer before releasing the parsed CA tree for established
     * connections. This keeps eight simultaneous WiFiSSLClient slots viable
     * without sharing trust state between slots that may use different CAs.
     */
    mbedtls_ssl_conf_ca_chain(&connection.config, nullptr, nullptr);
    mbedtls_x509_crt_free(&connection.ca);
    connection.ca_initialized = false;
}

bool configure_tls(
    TlsConnection &connection,
    const char *hostname,
    const uint8_t *ca_certificate,
    size_t ca_certificate_size,
    const uint8_t *client_certificate,
    size_t client_certificate_size,
    const uint8_t *client_private_key,
    size_t client_private_key_size)
{
    const bool has_client_credentials =
        client_certificate != nullptr ||
        client_certificate_size != 0U ||
        client_private_key != nullptr ||
        client_private_key_size != 0U;
    if (ca_certificate == nullptr ||
        ca_certificate_size == 0U ||
        (has_client_credentials &&
         (client_certificate == nullptr ||
          client_certificate_size == 0U ||
          client_private_key == nullptr ||
          client_private_key_size == 0U))) {
        return false;
    }
    mbedtls_ssl_config_init(&connection.config);
    connection.config_initialized = true;
    mbedtls_ssl_init(&connection.ssl);
    connection.ssl_initialized = true;
    mbedtls_x509_crt_init(&connection.ca);
    connection.ca_initialized = true;
    int result = mbedtls_x509_crt_parse(
        &connection.ca,
        ca_certificate,
        ca_certificate_size);
    if (result != 0) {
        connection.diagnostic_error = result;
        return false;
    }
    connection.diagnostic_phase = 31U;
    result = mbedtls_ssl_config_defaults(
        &connection.config,
        MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT);
    if (result != 0) {
        connection.diagnostic_error = result;
        return false;
    }
    mbedtls_ssl_conf_authmode(
        &connection.config,
        MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&connection.config, &connection.ca, nullptr);
    mbedtls_ssl_conf_rng(&connection.config, trng_random, nullptr);
    connection.diagnostic_phase = 38U;
    result = mbedtls_ssl_conf_max_frag_len(
        &connection.config,
        kTlsMaxFragmentLength);
    if (result != 0) {
        connection.diagnostic_error = result;
        return false;
    }
    if (has_client_credentials) {
        mbedtls_x509_crt_init(&connection.client_certificate);
        connection.client_certificate_initialized = true;
        result = mbedtls_x509_crt_parse_der(
            &connection.client_certificate,
            client_certificate,
            client_certificate_size);
        if (result != 0) {
            connection.diagnostic_phase = 35U;
            connection.diagnostic_error = result;
            return false;
        }
        mbedtls_pk_init(&connection.client_private_key);
        connection.client_private_key_initialized = true;
        result = mbedtls_pk_parse_key(
            &connection.client_private_key,
            client_private_key,
            client_private_key_size,
            nullptr,
            0U,
            trng_random,
            nullptr);
        if (result != 0) {
            connection.diagnostic_phase = 36U;
            connection.diagnostic_error = result;
            return false;
        }
        result = mbedtls_ssl_conf_own_cert(
            &connection.config,
            &connection.client_certificate,
            &connection.client_private_key);
        if (result != 0) {
            connection.diagnostic_phase = 37U;
            connection.diagnostic_error = result;
            return false;
        }
    }
    connection.diagnostic_phase = 32U;
    result = mbedtls_ssl_setup(&connection.ssl, &connection.config);
    if (result != 0) {
        connection.diagnostic_error = result;
        return false;
    }
    connection.diagnostic_phase = 33U;
    if (hostname != nullptr && hostname[0] != '\0' &&
        (result = mbedtls_ssl_set_hostname(&connection.ssl, hostname)) != 0) {
        connection.diagnostic_error = result;
        return false;
    }
    connection.diagnostic_phase = 34U;
    mbedtls_ssl_set_bio(
        &connection.ssl,
        &connection,
        ssl_send,
        ssl_recv,
        nullptr);
    return true;
}

void discard_tls_connection(TlsConnection &connection)
{
    if (connection.socket >= 0) {
        (void)closesocket(connection.socket);
    }
    connection.socket = -1;
    connection.connected = false;
    free_tls_context(connection);
    connection.has_peeked = false;
}

void mark_tls_peer_disconnected(TlsConnection &connection, int error)
{
    connection.connected = false;
    connection.diagnostic_phase = 51U;
    connection.diagnostic_error = error;
}

bool probe_tls_peer(TlsConnection &connection, bool *connected)
{
    if (connected == nullptr) {
        return false;
    }
    if (!connection.connected) {
        *connected = false;
        return true;
    }
    if (connection.has_peeked ||
        mbedtls_ssl_get_bytes_avail(&connection.ssl) != 0U) {
        *connected = true;
        return true;
    }

    fd_set readable = {};
    FD_SET(connection.socket, &readable);
    struct timeval timeout = {};
    const int selected = select(
        connection.socket + 1,
        &readable,
        nullptr,
        nullptr,
        &timeout);
    if (selected == 0 ||
        (selected < 0 && errno == EINTR)) {
        *connected = true;
        return true;
    }
    if (selected < 0) {
        mark_tls_peer_disconnected(connection, -errno);
        *connected = false;
        return true;
    }

    uint8_t byte = 0U;
    const int result = mbedtls_ssl_read(&connection.ssl, &byte, 1U);
    if (result > 0) {
        connection.peeked = byte;
        connection.has_peeked = true;
        *connected = true;
        return true;
    }
    if (result == MBEDTLS_ERR_SSL_WANT_READ ||
        result == MBEDTLS_ERR_SSL_WANT_WRITE) {
        *connected = true;
        return true;
    }

    mark_tls_peer_disconnected(connection, result);
    *connected = false;
    return true;
}

bool connect_common(
    TlsConnection *connection,
    const uint8_t address[4],
    uint16_t port,
    uint32_t timeout_ms,
    const char *hostname,
    const uint8_t *ca_certificate,
    size_t ca_certificate_size,
    const uint8_t *client_certificate,
    size_t client_certificate_size,
    const uint8_t *client_private_key,
    size_t client_private_key_size)
{
    if (connection == nullptr || connection->connected) {
        return false;
    }
    connection->diagnostic_phase = 1U;
    connection->diagnostic_error = 0;
    connection->verify_flags = 0U;
    connection->connection_timeout_units =
        static_cast<uint16_t>(timeout_ms / 10U);
    connection->peer_address_valid = false;
    connection->peer_port = 0U;
    record_heap_before_setup(*connection);
    uint8_t tcp_failure_phase = 21U;
    if (!open_tcp_socket(
            address, port, timeout_ms, &connection->socket,
            &tcp_failure_phase)) {
        connection->diagnostic_phase = tcp_failure_phase;
        connection->diagnostic_error = -errno;
        record_heap_failure(*connection);
        record_heap_after_cleanup(*connection);
        return false;
    }
    struct sockaddr_in peer = {};
    socklen_t peer_length = sizeof(peer);
    if (getpeername(
            connection->socket,
            reinterpret_cast<struct sockaddr *>(&peer),
            &peer_length) == 0) {
        const uint32_t peer_ip = ntohl(peer.sin_addr.s_addr);
        connection->peer_address[0] = static_cast<uint8_t>(peer_ip >> 24U);
        connection->peer_address[1] = static_cast<uint8_t>(peer_ip >> 16U);
        connection->peer_address[2] = static_cast<uint8_t>(peer_ip >> 8U);
        connection->peer_address[3] = static_cast<uint8_t>(peer_ip);
        connection->peer_port = ntohs(peer.sin_port);
        connection->peer_address_valid = true;
    }
    connection->diagnostic_phase = 3U;
    record_heap_before_setup(*connection);
    if (!configure_tls(
            *connection,
            hostname,
            ca_certificate,
            ca_certificate_size,
            client_certificate,
            client_certificate_size,
            client_private_key,
            client_private_key_size)) {
        record_heap_failure(*connection);
        discard_tls_connection(*connection);
        record_heap_after_cleanup(*connection);
        return false;
    }
    record_heap_before_handshake(*connection);
    connection->handshake_elapsed_ms = 0U;
    connection->diagnostic_phase = 4U;
    const unsigned long started_at = millis();
    int result = 0;
    while (!mbedtls_ssl_is_handshake_over(&connection->ssl)) {
        /*
         * mbedtls_ssl_handshake() owns an internal loop and may call a
         * timeout-capable BIO repeatedly before returning. Advance exactly
         * one public mbedTLS state-machine step here so the bridge's
         * caller-visible deadline remains authoritative on lwIP.
         */
        result = mbedtls_ssl_handshake_step(&connection->ssl);
        if (result == 0) {
            if (static_cast<uint32_t>(millis() - started_at) >= timeout_ms) {
                result = MBEDTLS_ERR_SSL_TIMEOUT;
                break;
            }
            continue;
        }
        if (result != MBEDTLS_ERR_SSL_WANT_READ &&
            result != MBEDTLS_ERR_SSL_WANT_WRITE) {
            break;
        }
        const uint32_t elapsed_ms = static_cast<uint32_t>(millis() - started_at);
        if (elapsed_ms >= timeout_ms) {
            result = MBEDTLS_ERR_SSL_TIMEOUT;
            break;
        }
        /*
         * The bridge profile's finite-timeout lwIP select() waits have
         * repeatedly extended a 50 ms request to about 20 s on hardware.
         * ssl_recv() only uses a zero-timeout readiness probe, then this
         * cooperative wait leaves the task schedulable while the monotonic
         * bridge deadline remains exact.
        */
        delay(1);
    }
    record_handshake_elapsed(*connection, started_at);
    const uint32_t verify_flags =
        mbedtls_ssl_get_verify_result(&connection->ssl);
    if (result != 0 || verify_flags != 0U) {
        connection->verify_flags = verify_flags;
        connection->diagnostic_error = result != 0
            ? result
            : -static_cast<int32_t>(verify_flags);
        if (connection->diagnostic_phase < 40U) {
            connection->diagnostic_phase = result != 0 ? 4U : 5U;
        }
        record_heap_failure(*connection);
        discard_tls_connection(*connection);
        record_heap_after_cleanup(*connection);
        return false;
    }
    connection->diagnostic_phase = 6U;
    connection->connected = true;
    release_handshake_ca(*connection);
    return true;
}

bool create_tls(void *, int *handle)
{
    if (handle == nullptr) {
        return false;
    }
    for (size_t index = 0U; index < kTlsConnectionCount; ++index) {
        if (!connections[index].allocated) {
            connections[index] = {};
            connections[index].allocated = true;
            *handle = static_cast<int>(index);
            return true;
        }
    }
    return false;
}

bool close_tls(void *, int handle)
{
    TlsConnection *connection = get_connection(handle);
    if (connection == nullptr) {
        return false;
    }
    // WiFiClientSecure::stop() in the ESP32-S3 baseline closes the BSD
    // socket directly. Preserve that behavior instead of queueing a new TLS
    // alert after the caller has finished using this slot.
    discard_tls_connection(*connection);
    *connection = {};
    return true;
}

bool connect_hostname_tls(
    void *,
    int handle,
    const char *hostname,
    uint16_t port,
    uint32_t timeout_ms,
    const uint8_t *ca_certificate,
    size_t ca_certificate_size,
    const uint8_t *client_certificate,
    size_t client_certificate_size,
    const uint8_t *client_private_key,
    size_t client_private_key_size)
{
    TlsConnection *connection = get_connection(handle);
    if (connection == nullptr || hostname == nullptr) {
        return false;
    }
    connection->resolved_address_valid = false;
    ip_addr_t resolved = {};
    const err_t raw_error = netconn_gethostbyname(hostname, &resolved);
    if (raw_error != ERR_OK || !IP_IS_V4(&resolved)) {
        connection->diagnostic_phase = 1U;
        connection->diagnostic_error = static_cast<int32_t>(raw_error);
        return false;
    }
    uint8_t address[4] = {};
    const ip4_addr_t *resolved_v4 = ip_2_ip4(&resolved);
    address[0] = ip4_addr1(resolved_v4);
    address[1] = ip4_addr2(resolved_v4);
    address[2] = ip4_addr3(resolved_v4);
    address[3] = ip4_addr4(resolved_v4);
    memcpy(
        connection->resolved_address,
        address,
        sizeof(connection->resolved_address));
    connection->resolved_address_valid = true;
    return connect_common(
        connection,
        address,
        port,
        timeout_ms,
        hostname,
        ca_certificate,
        ca_certificate_size,
        client_certificate,
        client_certificate_size,
        client_private_key,
        client_private_key_size);
}

bool connect_ip_tls(
    void *,
    int handle,
    const uint8_t address[4],
    uint16_t port,
    uint32_t timeout_ms,
    const uint8_t *ca_certificate,
    size_t ca_certificate_size,
    const uint8_t *client_certificate,
    size_t client_certificate_size,
    const uint8_t *client_private_key,
    size_t client_private_key_size)
{
    if (address == nullptr) {
        return false;
    }
    TlsConnection *connection = get_connection(handle);
    if (connection == nullptr) {
        return false;
    }
    connection->resolved_address_valid = false;
    memcpy(
        connection->resolved_address,
        address,
        sizeof(connection->resolved_address));
    connection->resolved_address_valid = true;
    char ip_text[16] = {};
    const int written = snprintf(
        ip_text,
        sizeof(ip_text),
        "%u.%u.%u.%u",
        static_cast<unsigned int>(address[0]),
        static_cast<unsigned int>(address[1]),
        static_cast<unsigned int>(address[2]),
        static_cast<unsigned int>(address[3]));
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(ip_text)) {
        return false;
    }
    return connect_common(
        connection,
        address,
        port,
        timeout_ms,
        ip_text,
        ca_certificate,
        ca_certificate_size,
        client_certificate,
        client_certificate_size,
        client_private_key,
        client_private_key_size);
}

bool send_tls(void *, int handle, const uint8_t *data, size_t size, size_t *sent)
{
    TlsConnection *connection = get_connection(handle);
    if (connection == nullptr || data == nullptr || sent == nullptr ||
        !connection->connected) {
        return false;
    }
    *sent = 0U;
    const unsigned long started_at = millis();
    while (*sent < size) {
        const int result = mbedtls_ssl_write(
            &connection->ssl,
            data + *sent,
            size - *sent);
        if (result > 0) {
            *sent += static_cast<size_t>(result);
            continue;
        }
        if (result != MBEDTLS_ERR_SSL_WANT_READ &&
            result != MBEDTLS_ERR_SSL_WANT_WRITE) {
            mark_tls_peer_disconnected(*connection, result);
            return false;
        }
        if (millis() - started_at >= kSocketTimeoutMs) {
            return false;
        }
        delay(1);
    }
    return true;
}

bool receive_tls(
    void *,
    int handle,
    uint8_t *data,
    size_t capacity,
    size_t *received)
{
    TlsConnection *connection = get_connection(handle);
    if (connection == nullptr || data == nullptr || received == nullptr ||
        capacity == 0U || !connection->connected) {
        return false;
    }
    *received = 0U;
    if (connection->has_peeked) {
        data[(*received)++] = connection->peeked;
        connection->has_peeked = false;
        if (*received == capacity) {
            return true;
        }
    }
    do {
        const int result = mbedtls_ssl_read(
            &connection->ssl,
            data + *received,
            capacity - *received);
        if (result > 0) {
            *received += static_cast<size_t>(result);
#if BL616CL_TLS_COALESCE_READ
            // A 1023-byte WiFiS3 request can leave one byte of a 1024-byte
            // TLS record. Fill from subsequent records using the existing
            // nonblocking BIO; never wait/retry on WANT_READ/WANT_WRITE.
            continue;
#else
            return true;
#endif
        }
        if (result == MBEDTLS_ERR_SSL_WANT_READ ||
            result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return true;
        }
        mark_tls_peer_disconnected(*connection, result);
#if BL616CL_TLS_COALESCE_READ
        // Deliver bytes already obtained before reporting the peer closure.
        return result == 0 || *received != 0U;
#else
        return result == 0;
#endif
    } while (*received < capacity);
    return true;
}

#if BL616CL_TLS_CACHED_AVAILABLE
bool cached_available_tls(void *, int handle, size_t *bytes)
{
    TlsConnection *connection = get_connection(handle);
    if (connection == nullptr || bytes == nullptr || !connection->connected) {
        return false;
    }
    // These fields belong to the AT task. Do not probe the socket or SSL read:
    // a cache miss must acquire the normal RF window before network work.
    const size_t cached = mbedtls_ssl_get_bytes_avail(&connection->ssl) +
        (connection->has_peeked ? 1U : 0U);
    if (cached == 0U) {
        return false;
    }
    *bytes = cached;
    return true;
}
#endif

bool available_tls(void *, int handle, size_t *bytes)
{
    TlsConnection *connection = get_connection(handle);
    if (connection == nullptr || bytes == nullptr) {
        return false;
    }
    bool connected = false;
    if (!probe_tls_peer(*connection, &connected)) {
        return false;
    }
    if (!connected) {
        *bytes = 0U;
        return true;
    }
    const size_t decrypted = mbedtls_ssl_get_bytes_avail(&connection->ssl);
    // The probe's saved byte and the decrypted remainder are both immediately
    // readable. Reporting only the saved byte undercounts the current record.
    *bytes = decrypted + (connection->has_peeked ? 1U : 0U);
    return true;
}

bool peek_tls(void *, int handle, int *value)
{
    TlsConnection *connection = get_connection(handle);
    if (connection == nullptr || value == nullptr) {
        return false;
    }
    if (!connection->connected) {
        *value = -1;
        return true;
    }
    if (!connection->has_peeked &&
        mbedtls_ssl_get_bytes_avail(&connection->ssl) != 0U) {
        uint8_t byte = 0U;
        const int result = mbedtls_ssl_read(&connection->ssl, &byte, 1U);
        if (result > 0) {
            connection->peeked = byte;
            connection->has_peeked = true;
        } else if (result != MBEDTLS_ERR_SSL_WANT_READ &&
                   result != MBEDTLS_ERR_SSL_WANT_WRITE) {
            mark_tls_peer_disconnected(*connection, result);
        }
    }
    bool connected = false;
    if (!probe_tls_peer(*connection, &connected)) {
        return false;
    }
    if (!connected || !connection->has_peeked) {
        *value = -1;
        return true;
    }
    *value = connection->peeked;
    return true;
}

bool connected_tls(void *, int handle, bool *connected)
{
    TlsConnection *connection = get_connection(handle);
    if (connection == nullptr || connected == nullptr) {
        return false;
    }
    return probe_tls_peer(*connection, connected);
}

bool state_tls(
    void *,
    int handle,
    uint8_t remote_address[4],
    uint16_t *remote_port,
    uint16_t *local_port)
{
    TlsConnection *connection = get_connection(handle);
    if (connection == nullptr || remote_address == nullptr ||
        remote_port == nullptr || local_port == nullptr ||
        !connection->connected) {
        return false;
    }
    struct sockaddr_in remote = {};
    socklen_t remote_length = sizeof(remote);
    struct sockaddr_in local = {};
    socklen_t local_length = sizeof(local);
    if (getpeername(
            connection->socket,
            reinterpret_cast<struct sockaddr *>(&remote),
            &remote_length) != 0 ||
        getsockname(
            connection->socket,
            reinterpret_cast<struct sockaddr *>(&local),
            &local_length) != 0) {
        return false;
    }
    const uint32_t remote_ip = ntohl(remote.sin_addr.s_addr);
    remote_address[0] = static_cast<uint8_t>(remote_ip >> 24U);
    remote_address[1] = static_cast<uint8_t>(remote_ip >> 16U);
    remote_address[2] = static_cast<uint8_t>(remote_ip >> 8U);
    remote_address[3] = static_cast<uint8_t>(remote_ip);
    *remote_port = ntohs(remote.sin_port);
    *local_port = ntohs(local.sin_port);
    return true;
}

bool local_address_tls(void *, int handle, uint8_t address[4])
{
    TlsConnection *connection = get_connection(handle);
    if (connection == nullptr || address == nullptr || !connection->connected) {
        return false;
    }
    struct sockaddr_in local = {};
    socklen_t length = sizeof(local);
    if (getsockname(connection->socket,
                    reinterpret_cast<struct sockaddr *>(&local), &length) != 0) {
        return false;
    }
    const uint32_t ip = ntohl(local.sin_addr.s_addr);
    address[0] = static_cast<uint8_t>(ip >> 24U);
    address[1] = static_cast<uint8_t>(ip >> 16U);
    address[2] = static_cast<uint8_t>(ip >> 8U);
    address[3] = static_cast<uint8_t>(ip);
    return true;
}

bool flush_tls(void *, int handle)
{
    return get_connection(handle) != nullptr;
}

bool get_diagnostic_tls(
    void *,
    int handle,
    uint8_t *phase,
    int32_t *error)
{
    TlsConnection *connection = get_connection(handle);
    if (connection == nullptr || phase == nullptr || error == nullptr) {
        return false;
    }
    *phase = connection->diagnostic_phase;
    *error = connection->diagnostic_error;
    return true;
}

bool get_verify_flags_tls(void *, int handle, uint32_t *flags)
{
    TlsConnection *connection = get_connection(handle);
    if (connection == nullptr || flags == nullptr) {
        return false;
    }
    *flags = connection->verify_flags;
    return true;
}

bool get_heap_diagnostic_tls(void *, int handle, TlsHeapDiagnostic *diagnostic)
{
    TlsConnection *connection = get_connection(handle);
    if (connection == nullptr || diagnostic == nullptr) {
        return false;
    }
    diagnostic->before_handshake_free_bytes =
        static_cast<uint32_t>(connection->heap_before_handshake_units) *
        kHeapDiagnosticUnitBytes;
    diagnostic->failure_free_bytes =
        static_cast<uint32_t>(connection->heap_failure_units) *
        kHeapDiagnosticUnitBytes;
    diagnostic->after_cleanup_free_bytes =
        static_cast<uint32_t>(connection->heap_after_cleanup_units) *
        kHeapDiagnosticUnitBytes;
    diagnostic->connection_timeout_ms =
        static_cast<uint32_t>(connection->connection_timeout_units) * 10U;
    diagnostic->handshake_elapsed_ms = connection->handshake_elapsed_ms;
    diagnostic->before_setup_ocram_free_bytes =
        static_cast<uint32_t>(connection->ocram_before_setup_units) *
        kHeapDiagnosticUnitBytes;
    diagnostic->before_setup_wram_free_bytes =
        static_cast<uint32_t>(connection->wram_before_setup_units) *
        kHeapDiagnosticUnitBytes;
    diagnostic->failure_ocram_free_bytes =
        static_cast<uint32_t>(connection->ocram_failure_units) *
        kHeapDiagnosticUnitBytes;
    diagnostic->failure_wram_free_bytes =
        static_cast<uint32_t>(connection->wram_failure_units) *
        kHeapDiagnosticUnitBytes;
    diagnostic->after_cleanup_ocram_free_bytes =
        static_cast<uint32_t>(connection->ocram_after_cleanup_units) *
        kHeapDiagnosticUnitBytes;
    diagnostic->after_cleanup_wram_free_bytes =
        static_cast<uint32_t>(connection->wram_after_cleanup_units) *
        kHeapDiagnosticUnitBytes;
    diagnostic->observed_ocram_free_bytes =
        static_cast<uint32_t>(heap_free_units(MM_HEAP_OCRAM_0)) *
        kHeapDiagnosticUnitBytes;
    diagnostic->observed_wram_free_bytes =
        static_cast<uint32_t>(heap_free_units(MM_HEAP_WRAM_0)) *
        kHeapDiagnosticUnitBytes;
    diagnostic->before_setup_wram_max_block_bytes =
        static_cast<uint32_t>(connection->wram_max_before_setup_units) *
        kHeapDiagnosticUnitBytes;
    diagnostic->failure_wram_max_block_bytes =
        static_cast<uint32_t>(connection->wram_max_failure_units) *
        kHeapDiagnosticUnitBytes;
    diagnostic->after_cleanup_wram_max_block_bytes =
        static_cast<uint32_t>(connection->wram_max_after_cleanup_units) *
        kHeapDiagnosticUnitBytes;
    diagnostic->observed_wram_max_block_bytes =
        static_cast<uint32_t>(wram_max_free_block_units()) *
        kHeapDiagnosticUnitBytes;
#if MEMP_STATS
    diagnostic->failure_tcp_pcb_used = connection->failure_tcp_pcb_used;
    diagnostic->failure_tcp_seg_used = connection->failure_tcp_seg_used;
    diagnostic->failure_netconn_used = connection->failure_netconn_used;
    diagnostic->observed_tcp_pcb_used =
        lwip_stats.memp[MEMP_TCP_PCB]->used;
    diagnostic->observed_tcp_seg_used =
        lwip_stats.memp[MEMP_TCP_SEG]->used;
    diagnostic->observed_netconn_used =
        lwip_stats.memp[MEMP_NETCONN]->used;
    diagnostic->tcp_seg_allocation_errors =
        lwip_stats.memp[MEMP_TCP_SEG]->err;
#endif
#if MEM_STATS
    diagnostic->failure_lwip_heap_used =
        connection->failure_lwip_heap_used;
    diagnostic->observed_lwip_heap_used = lwip_stats.mem.used;
    diagnostic->lwip_heap_allocation_errors = lwip_stats.mem.err;
#endif
    // Lists and segment chains can change on the TCP/IP thread. Only copy
    // aggregate counters while locked; formatting and UART output happen later.
#if BL616CL_TLS_FLOW_DIAGNOSTIC
    // Resolve the socket tuple before taking the core lock: BSD calls can
    // themselves enter the TCP/IP thread. Never export the addresses.
    struct sockaddr_in flow_local = {}, flow_peer = {};
    socklen_t flow_local_size = sizeof(flow_local), flow_peer_size = sizeof(flow_peer);
    const bool flow_tuple_valid = connection->socket >= 0 &&
        getsockname(connection->socket, reinterpret_cast<struct sockaddr *>(&flow_local), &flow_local_size) == 0 &&
        getpeername(connection->socket, reinterpret_cast<struct sockaddr *>(&flow_peer), &flow_peer_size) == 0 &&
        flow_local_size == sizeof(flow_local) && flow_peer_size == sizeof(flow_peer) &&
        flow_local.sin_family == AF_INET && flow_peer.sin_family == AF_INET;
    diagnostic->flow.values[15] = connection->ssl_initialized
        ? mbedtls_ssl_get_bytes_avail(&connection->ssl) : 0;
    diagnostic->flow.values[16] = connection->has_peeked ? 1 : 0;
    diagnostic->flow.values[17] = flow_tuple_valid ? 1 : 0;
#endif
    static_assert(TIME_WAIT == 10, "TCP diagnostic state order changed");
    LOCK_TCPIP_CORE();
    const struct tcp_pcb *heads[] = {
        tcp_bound_pcbs, tcp_active_pcbs, tcp_tw_pcbs,
    };
#if BL616CL_TLS_FLOW_DIAGNOSTIC
    if (flow_tuple_valid) {
        capture_tcp_flow(heads, 3, [&](const struct tcp_pcb &pcb) {
            return IP_IS_V4_VAL(pcb.local_ip) && IP_IS_V4_VAL(pcb.remote_ip) &&
                pcb.local_port == ntohs(flow_local.sin_port) && pcb.remote_port == ntohs(flow_peer.sin_port) &&
                ip4_addr_get_u32(ip_2_ip4(&pcb.local_ip)) == flow_local.sin_addr.s_addr &&
                ip4_addr_get_u32(ip_2_ip4(&pcb.remote_ip)) == flow_peer.sin_addr.s_addr;
        }, diagnostic->flow);
    }
#endif
    for (const struct tcp_pcb *head : heads) {
        for (const struct tcp_pcb *pcb = head; pcb != nullptr; pcb = pcb->next) {
            ++diagnostic->tcp_state_count[pcb->state];
            for (const struct tcp_seg *seg = pcb->unacked;
                 seg != nullptr; seg = seg->next) {
                diagnostic->tcp_unacked_pbuf_bytes += seg->p->tot_len;
            }
            for (const struct tcp_seg *seg = pcb->unsent;
                 seg != nullptr; seg = seg->next) {
                diagnostic->tcp_unsent_pbuf_bytes += seg->p->tot_len;
            }
#if TCP_QUEUE_OOSEQ
            for (const struct tcp_seg *seg = pcb->ooseq;
                 seg != nullptr; seg = seg->next) {
                diagnostic->tcp_ooseq_pbuf_bytes += seg->p->tot_len;
            }
#endif
            if (pcb->nrtx > diagnostic->tcp_max_retransmissions) {
                diagnostic->tcp_max_retransmissions = pcb->nrtx;
            }
        }
    }
    for (const struct tcp_pcb_listen *pcb = tcp_listen_pcbs.listen_pcbs;
         pcb != nullptr; pcb = pcb->next) {
        ++diagnostic->tcp_state_count[LISTEN];
    }
    UNLOCK_TCPIP_CORE();
    return true;
}

bool get_endpoint_diagnostic_tls(
    void *,
    int handle,
    TlsEndpointDiagnostic *diagnostic)
{
    TlsConnection *connection = get_connection(handle);
    if (connection == nullptr || diagnostic == nullptr) {
        return false;
    }
    memcpy(
        diagnostic->resolved_address,
        connection->resolved_address,
        sizeof(diagnostic->resolved_address));
    memcpy(
        diagnostic->peer_address,
        connection->peer_address,
        sizeof(diagnostic->peer_address));
    diagnostic->peer_port = connection->peer_port;
    diagnostic->resolved_valid = connection->resolved_address_valid;
    diagnostic->peer_valid = connection->peer_address_valid;
    return true;
}

} // namespace

// Called by the audited SDK patch only when a TLS setup allocation fails.
// Match the actual SSL context so unrelated SDK clients cannot change a slot.
// Preserve the first failure if several handshake structures fail together.
extern "C" void bridge_mbedtls_alloc_failed(
    const void *ssl, unsigned stage, size_t requested)
{
    (void)requested;
    for (auto &connection : connections) {
        if (ssl == &connection.ssl && connection.ssl_initialized &&
            connection.diagnostic_phase == 32U && stage >= 1U && stage <= 5U) {
            // 51: RX record, 52: TX record, 53: transform, 54: session,
            // 55: handshake. SSLCLIENTDIAG retains its existing field layout.
            connection.diagnostic_phase = static_cast<uint8_t>(50U + stage);
            break;
        }
    }
}

const TlsClientBackend &bouffalo_tls_client_backend()
{
    static const TlsClientBackend backend = {
        nullptr,
        create_tls,
        close_tls,
        connect_hostname_tls,
        connect_ip_tls,
        send_tls,
        receive_tls,
        available_tls,
        peek_tls,
        connected_tls,
        state_tls,
        flush_tls,
        get_diagnostic_tls,
        get_verify_flags_tls,
        get_heap_diagnostic_tls,
        get_endpoint_diagnostic_tls,
        local_address_tls,
#if BL616CL_TLS_CACHED_AVAILABLE
        cached_available_tls,
#endif
    };
    return backend;
}

} // namespace bridge
