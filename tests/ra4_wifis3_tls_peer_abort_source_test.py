#!/usr/bin/env python3
"""Keep TLS peer-abort recovery on the real WiFiSSLClient path."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SKETCH = (
    ROOT
    / "examples"
    / "ra4-wifis3-tls-peer-abort-gate"
    / "ra4-wifis3-tls-peer-abort-gate.ino.in"
).read_text(encoding="ascii")
RUNNER = (
    ROOT / "tools" / "run_ra4_wifis3_tls_peer_abort_gate.py"
).read_text(encoding="ascii")
BACKEND = (
    ROOT / "UNOR4BL616CLBridge" / "bouffalo_tls_client_backend.cpp"
).read_text(encoding="ascii")


def require(source: str, text: str) -> None:
    if text not in source:
        raise RuntimeError(f"TLS peer-abort gate missing: {text}")


for required in (
    "WiFiSSLClient client;",
    "client.connected()",
    "RA4_TLS_PEER_ABORT:CONNECTED_AFTER_ABORT=",
    "RA4_TLS_PEER_ABORT:WRITE_AFTER_ABORT=",
    "RA4_TLS_PEER_ABORT:ABORT_DETECTED_PASS",
    "RA4_TLS_PEER_ABORT:RECOVERY_ECHO_PASS",
    "RA4_TLS_PEER_ABORT:RESULT=PASS",
):
    require(SKETCH, required)

for required in (
    "SO_LINGER",
    "TLS_PEER_ABORT:TCP_RST_SENT",
    "TLS_PEER_ABORT:RECOVERY_ECHO_PASS",
    "prepare_wifis3_result_error_overlay.py",
    '"--library",',
    "str(overlay_library)",
    "RA4_WIFIS3_TLS_PEER_ABORT_GATE_PASS",
    "RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_PASS",
):
    require(RUNNER, required)

for required in (
    "bool probe_tls_peer(",
    "mbedtls_ssl_get_bytes_avail",
    "connection.has_peeked = true;",
    "mark_tls_peer_disconnected(*connection, result);",
    "return probe_tls_peer(*connection, connected);",
):
    require(BACKEND, required)

print("RA4_WIFIS3_TLS_PEER_ABORT_SOURCE_PASS")
