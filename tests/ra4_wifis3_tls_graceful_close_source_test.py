#!/usr/bin/env python3
"""Keep the TLS close_notify gate on the real WiFiSSLClient path."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SKETCH = (
    ROOT
    / "examples"
    / "ra4-wifis3-tls-graceful-close-gate"
    / "ra4-wifis3-tls-graceful-close-gate.ino.in"
).read_text(encoding="ascii")
RUNNER = (
    ROOT / "tools" / "run_ra4_wifis3_tls_graceful_close_gate.py"
).read_text(encoding="ascii")
BACKEND = (
    ROOT / "UNOR4BL616CLBridge" / "bouffalo_tls_client_backend.cpp"
).read_text(encoding="ascii")


def require(source: str, text: str) -> None:
    if text not in source:
        raise RuntimeError(f"TLS graceful-close gate missing: {text}")


for required in (
    "WiFiSSLClient client;",
    "client.connected()",
    "client.available()",
    "client.read()",
    "RA4_TLS_GRACEFUL_CLOSE:CLOSE_NOTIFY_PASS",
    "RA4_TLS_GRACEFUL_CLOSE:WRITE_AFTER_CLOSE=",
    "RA4_TLS_GRACEFUL_CLOSE:RECOVERY_ECHO_PASS",
    "RA4_TLS_GRACEFUL_CLOSE:RESULT=PASS",
    "@TLS_EXPECT_ABRUPT_CLOSE@",
    "RA4_TLS_GRACEFUL_CLOSE:TAIL_DATA_PASS",
    "RA4_TLS_GRACEFUL_CLOSE:BARE_FIN_PASS",
    "@TLS_CLOSE_ROUNDS@",
    "@TLS_MIXED_CLOSE@",
    "sample_idle_heap()",
    "AT+SSLCLIENTDIAG=%d",
    "RA4_TLS_GRACEFUL_CLOSE:CYCLE_PASS=",
):
    require(SKETCH, required)

for required in (
    "first_tls.unwrap()",
    "TLS_GRACEFUL_CLOSE_PEER:CLOSE_NOTIFY_SENT",
    "TLS_GRACEFUL_CLOSE_PEER:RECOVERY_ECHO_PASS",
    "TLS_GRACEFUL_CLOSE_PEER:CLIENT_CLOSE_NOTIFY_RECEIVED",
    "prepare_wifis3_result_error_overlay.py",
    "RA4_WIFIS3_TLS_GRACEFUL_CLOSE_GATE_PASS",
    "RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_PASS",
    "first_tls.detach()",
    "first_raw.shutdown(socket.SHUT_WR)",
    "TLS_GRACEFUL_CLOSE_PEER:CLIENT_FIN_RECEIVED",
    "suppress_ragged_eofs=False",
    "second_tls.recv(1)",
    '"--close-mode"',
    '"--rounds"',
    "def validate_result(",
):
    require(RUNNER, required)

for required in (
    "mbedtls_ssl_read(&connection->ssl",
    "if (result == 0)",
    "mark_tls_peer_disconnected(*connection, result);",
    "bool probe_tls_peer(",
):
    require(BACKEND, required)

print("RA4_WIFIS3_TLS_GRACEFUL_CLOSE_SOURCE_PASS")
