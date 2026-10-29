#!/usr/bin/env python3
"""Guard the BL616CL nonblocking TCP completion contract at source level."""

from __future__ import annotations

from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parents[1]
SOURCES = (
    ROOT_DIR / "UNOR4BL616CLBridge" / "bouffalo_tcp_client_backend.cpp",
    ROOT_DIR / "UNOR4BL616CLBridge" / "bouffalo_tls_client_backend.cpp",
)


def require(source: str, text: str, message: str) -> None:
    if text not in source:
        raise RuntimeError(message)


def main() -> int:
    tcp_source = SOURCES[0].read_text(encoding="ascii")
    require(
        tcp_source,
        "bool set_socket_timeouts(int handle, uint32_t timeout_ms)",
        "TCP backend must configure a bounded blocking connect timeout",
    )
    require(
        tcp_source,
        "if (!set_socket_timeouts(handle, timeout_ms))",
        "TCP backend must apply its caller timeout before connect",
    )
    require(
        tcp_source,
        "sizeof(remote)) == 0;",
        "TCP backend must expose success only after blocking connect succeeds",
    )
    if "const int selected = select(" in tcp_source:
        raise RuntimeError(
            "TCP backend must not use the unreliable nonblocking "
            "select/SO_ERROR completion path"
        )

    tls_source = SOURCES[1].read_text(encoding="ascii")
    require(
        tls_source,
        "if (result != 0 && errno != EINPROGRESS)",
        "TLS default backend must reject connect errors other than EINPROGRESS",
    )
    require(
        tls_source,
        "const int selected = select(",
        "TLS default backend must retain its nonblocking completion path",
    )
    require(
        tls_source,
        "SO_ERROR",
        "TLS default backend must confirm nonblocking connect with SO_ERROR",
    )
    require(
        tls_source,
        "constexpr uint32_t kTlsHandshakeWaitSliceMs = 50U;",
        "TLS backend must use bounded waits while the handshake is pending",
    )
    require(
        tls_source,
        "ssl_recv() only uses a zero-timeout readiness probe",
        "TLS backend must document why it avoids finite-timeout lwIP select",
    )
    require(
        tls_source,
        "MBEDTLS_ERR_SSL_TIMEOUT",
        "TLS backend must report an explicit TLS handshake timeout",
    )
    require(
        tls_source,
        "void record_handshake_elapsed(",
        "TLS backend must retain a bridge-owned handshake elapsed diagnostic",
    )
    require(
        tls_source,
        "record_handshake_elapsed(*connection, started_at);",
        "TLS backend must capture elapsed time before it releases a failed TLS slot",
    )
    require(
        tls_source,
        "int ssl_recv(void *context, unsigned char *data, size_t size)",
        "TLS backend must provide a nonblocking receive BIO",
    )
    require(
        tls_source,
        "struct timeval timeout = {};",
        "TLS receive BIO must not let mbedTLS own a blocking socket timeout",
    )
    require(
        tls_source,
        "ssl_send,\n        ssl_recv,\n        nullptr",
        "TLS backend must select the nonblocking receive BIO",
    )
    require(
        tls_source,
        "mbedtls_ssl_handshake_step(&connection->ssl)",
        "TLS handshake must advance one state-machine step per deadline check",
    )
    require(
        tls_source,
        "while (!mbedtls_ssl_is_handshake_over(&connection->ssl))",
        "TLS handshake must retain bridge-owned deadline control",
    )
    require(
        tls_source,
        "delay(1);",
        "TLS handshake must cooperatively yield while polling its deadline",
    )
    if "wait_for_tls_socket_event(" in tls_source:
        raise RuntimeError(
            "TLS handshake must not use the BL616CL finite-timeout select path"
        )
    require(
        tls_source,
        "Reassert FIONBIO after select/SO_ERROR",
        "TLS backend must restore nonblocking I/O after TCP connect completes",
    )
    require(
        tls_source,
        "!set_socket_timeouts(socket, kTlsHandshakeWaitSliceMs)",
        "TLS backend must retain a short socket-timeout fallback when lwIP "
        "does not preserve FIONBIO",
    )
    require(
        tls_source,
        "BL616CL_TLS_BLOCKING_CONNECT_EXPERIMENT",
        "TLS backend must retain the guarded blocking-connect hardware experiment",
    )
    blocking_branch = tls_source.split(
        "#if BL616CL_TLS_BLOCKING_CONNECT_EXPERIMENT",
        1,
    )[1].split("#else", 1)[0]
    if "FIONBIO" in blocking_branch:
        raise RuntimeError(
            "TLS blocking-connect experiment must not leave the socket "
            "in nonblocking mode"
        )
    default_branch = tls_source.split("#else", 1)[1].split("#endif", 1)[0]
    if "nonblocking = 0U" in default_branch:
        raise RuntimeError(
            "TLS default path must not restore blocking I/O after connect"
        )
    if default_branch.count("ioctlsocket(socket, FIONBIO, &nonblocking)") < 2:
        raise RuntimeError(
            "TLS default path must reassert FIONBIO after connect completion"
        )
    print("BL616CL_TCP_CONNECT_COMPLETION_SOURCE_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
