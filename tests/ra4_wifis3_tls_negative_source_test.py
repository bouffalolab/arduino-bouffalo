#!/usr/bin/env python3
"""Keep the TLS untrusted-CA hardware gate on the real WiFiS3 path."""

from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parents[1]
RUNNER = ROOT_DIR / "tools" / "run_ra4_wifis3_tls_gate.py"


def require(source: str, text: str, message: str) -> None:
    if text not in source:
        raise RuntimeError(message)


def main() -> int:
    source = RUNNER.read_text(encoding="ascii")
    require(
        source,
        '"--expect-untrusted-ca"',
        "TLS runner must expose the untrusted-CA gate",
    )
    require(
        source,
        "client_ca_certificate = ca_certificate",
        "TLS runner must distinguish server and client CA material",
    )
    require(
        source,
        "RA4_TLS_CLIENT:RESULT=FAIL:TLS_CONNECT",
        "TLS runner must require the WiFiSSLClient failure marker",
    )
    require(
        source,
        "RA4_WIFIS3_TLS_UNTRUSTED_CA_GATE_PASS",
        "TLS runner must publish a distinct negative-gate marker",
    )
    require(
        source,
        "wait_for_peer_events(",
        "TLS runner must read the live peer event log for the rejection path",
    )
    require(
        source,
        '"--expect-nxdomain"',
        "TLS runner must expose the NXDOMAIN gate",
    )
    require(
        source,
        "RA4_WIFIS3_TLS_NXDOMAIN_GATE_PASS",
        "TLS runner must publish a distinct NXDOMAIN marker",
    )
    require(
        source,
        '"--expect-handshake-timeout"',
        "TLS runner must expose the handshake-timeout gate",
    )
    require(
        source,
        "RA4_WIFIS3_TLS_HANDSHAKE_TIMEOUT_GATE_PASS",
        "TLS runner must publish a distinct handshake-timeout marker",
    )
    require(
        source,
        '"--modem-response-timeout-ms"',
        "TLS runner must let its temporary Sketch outlive the core 10-second AT timeout",
    )
    require(
        source,
        '"--use-wifis3-result-error-overlay"',
        "TLS runner must support an isolated WiFiS3 RESULT_ERROR fix overlay",
    )
    require(
        source,
        "RA4_WIFIS3_RESULT_ERROR_OVERLAY_USED",
        "TLS runner must publish the isolated patched-core usage marker",
    )
    require(
        source,
        '"--trace-tls-modem"',
        "TLS runner must offer a credential-safe Modem raw trace",
    )
    require(
        source,
        "PAIRED_TLS_MODEM_TRACE_BEGIN",
        "TLS runner must emit the selected TLS-only Modem raw trace",
    )
    require(
        source,
        '"--require-ra4-deadline"',
        "TLS runner must make RA4 wall-clock deadline validation explicit",
    )
    print("RA4_WIFIS3_TLS_NEGATIVE_SOURCE_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
