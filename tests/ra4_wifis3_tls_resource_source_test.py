#!/usr/bin/env python3
"""Keep the TLS resource gate on real WiFiSSLClient and raw slot evidence."""

from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parents[1]
TEMPLATE = (
    ROOT_DIR
    / "examples"
    / "ra4-wifis3-tls-resource-gate"
    / "ra4-wifis3-tls-resource-gate.ino.in"
)
RUNNER = ROOT_DIR / "tools" / "run_ra4_wifis3_tls_resource_gate.py"


def require(source: str, text: str, message: str) -> None:
    if text not in source:
        raise RuntimeError(message)


def main() -> int:
    template = TEMPLATE.read_text(encoding="ascii")
    runner = RUNNER.read_text(encoding="ascii")
    require(
        template,
        "constexpr size_t kTlsClientCount = 8U;",
        "TLS resource Sketch must exercise all eight documented clients",
    )
    require(
        template,
        "@TLS_RESOURCE_ROUNDS@",
        "TLS resource Sketch must support repeated same-runtime resource cycles",
    )
    require(
        template,
        "bool run_round(",
        "TLS resource Sketch must isolate each repeated allocation/reuse cycle",
    )
    require(
        template,
        'constexpr char kIpPrefix[] = "STARTIP ";',
        "TLS resource Sketch must retain a direct-IP stability mode",
    )
    require(
        template,
        "client.connect(endpoint, port) == 1",
        "TLS resource Sketch direct-IP mode must still exercise WiFiSSLClient",
    )
    require(
        template,
        "for (size_t round = 1U; round <= kTlsResourceRounds; ++round)",
        "TLS resource Sketch must execute the requested resource cycles in one runtime",
    )
    require(
        template,
        "RA4_TLS_RESOURCE:NINTH_EXHAUSTED_PASS",
        "TLS resource Sketch must require ninth-slot exhaustion",
    )
    require(
        template,
        "kMiddleClientIndex = 3U",
        "TLS resource Sketch must close a non-edge TLS slot",
    )
    require(
        template,
        "RA4_TLS_RESOURCE:FAILED_HANDSHAKE_PASS",
        "TLS resource Sketch must exercise a real TLS handshake failure",
    )
    require(
        template,
        "RA4_TLS_RESOURCE:REUSE_SOCKET_PASS",
        "TLS resource Sketch must require middle-slot reuse",
    )
    require(
        template,
        "RA4_TLS_RESOURCE:REUSE_READ_PASS",
        "TLS resource Sketch must require post-reuse TLS data flow",
    )
    require(
        template,
        "kReadyRepeatIntervalMs",
        "TLS resource Sketch must repeat READY until the runner can observe it",
    )
    require(
        template,
        "RA4_TLS_RESOURCE:EIGHT_FAILURE_SLOT=",
        "TLS resource Sketch must report the first failing concurrent TLS slot",
    )
    require(
        template,
        "void print_tls_diagnostic",
        "TLS resource Sketch must retain the bridge failure diagnostic",
    )
    require(
        template,
        '"EIGHT_FAILURE"',
        "TLS resource Sketch must label the concurrent TLS failure diagnostic",
    )
    require(
        template,
        '"EIGHT_OPEN"',
        "TLS resource Sketch must retain diagnostics for successful TLS slots",
    )
    for marker in (
        "MIXED_TCP_PASS",
        "MIXED_UDP_16K_BUFFER_PASS",
        "MIXED_SERVER_ACCEPT_PASS",
        "MIXED_PEAK",
    ):
        require(template, marker, f"mixed resource probe must report {marker}")
    require(
        runner,
        "--mixed-probe",
        "mixed resource probe must remain opt-in",
    )
    require(
        runner,
        "save_gate_diagnostics(primary_error, ra4_output, peer_output)",
        "resource gate must retain filtered diagnostics",
    )
    require(
        (ROOT_DIR / "UNOR4BL616CLBridge" / "bouffalo_tls_client_backend.cpp")
        .read_text(encoding="ascii"),
        "record_heap_before_setup(*connection);",
        "TLS backend must sample heap pools before mbedTLS setup",
    )
    backend = (
        ROOT_DIR / "UNOR4BL616CLBridge" / "bouffalo_tls_client_backend.cpp"
    ).read_text(encoding="ascii")
    require(
        backend,
        "heap_free_units(MM_HEAP_OCRAM_0)",
        "TLS backend must capture OCRAM free space",
    )
    require(
        backend,
        "heap_free_units(MM_HEAP_WRAM_0)",
        "TLS backend must capture WRAM free space",
    )
    require(
        backend,
        "observed_ocram_free_bytes",
        "TLS backend must expose live per-heap memory evidence",
    )
    require(
        backend,
        "release_handshake_ca(*connection);",
        "TLS backend must release each established slot's handshake-only CA tree",
    )
    require(
        backend,
        "mbedtls_ssl_conf_ca_chain(&connection.config, nullptr, nullptr);",
        "TLS backend must detach the CA chain before releasing its parsed tree",
    )
    require(
        template,
        "query_tls_slot_closed",
        "TLS resource Sketch must verify close releases the raw bridge slot",
    )
    require(
        template,
        "close_all_clients_and_verify",
        "TLS resource Sketch must verify every raw TLS slot after a stress-round close",
    )
    require(
        template,
        "RA4_TLS_RESOURCE:ROUND=",
        "TLS resource Sketch must identify the stress round in close diagnostics",
    )
    require(
        template,
        ":CLOSE_FAILURE_SLOT=",
        "TLS resource Sketch must identify an unreleased TLS slot",
    )
    require(
        template,
        "RA4_TLS_RESOURCE:CLOSE_FAILURE_RESPONSE=",
        "TLS resource Sketch must preserve the unexpected close-query response",
    )
    require(
        runner,
        "prepare_wifis3_result_error_overlay.py",
        "TLS resource runner must use the repository-controlled WiFiS3 overlay",
    )
    require(
        runner,
        "TLS_RESOURCE_PEER:HANDSHAKE_REJECTED=9",
        "TLS resource runner must require the intentional ninth peer handshake rejection",
    )
    require(
        runner,
        "A client that rejects the intentionally untrusted CA can",
        "TLS resource peer must classify a client-side certificate abort as a failed handshake",
    )
    require(
        runner,
        "--rounds",
        "TLS resource runner must expose a configurable repeat count",
    )
    require(
        runner,
        "--direct-ip",
        "TLS resource runner must expose a DNS-independent stability mode",
    )
    require(
        runner,
        "TLS_RESOURCE_PEER:REUSE_EXCHANGE_PASS=",
        "TLS resource runner must require every requested post-reuse peer exchange",
    )
    require(
        runner,
        'str(args.rounds * 10 - 1)',
        "TLS resource runner must observe the final round's intentional rejection",
    )
    require(
        runner,
        'str(args.rounds * 10)',
        "TLS resource runner must observe the final round's successful reuse",
    )
    require(
        runner,
        'parser.add_argument("--rounds", required=True, type=int)',
        "Windows TLS resource peer must receive the requested cycle count",
    )
    require(
        runner,
        "max(90, args.rounds * 45)",
        "Windows TLS resource peer must retain a round-scaled lifetime",
    )
    require(
        runner,
        "FINAL_CARRIER_RA4_SCAN_RESTORE_PASS",
        "TLS resource runner must restore the final-carrier WiFiS3 scan Sketch",
    )
    require(
        runner,
        '"RA4_TLS_RESOURCE:MARKERS_TIMEOUT"',
        "TLS resource runner must retain serial evidence on a marker timeout",
    )
    print("RA4_WIFIS3_TLS_RESOURCE_SOURCE_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
