#!/usr/bin/env python3
"""Keep peer failure/recovery coverage explicit and bounded."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SKETCH = (ROOT / "examples" / "ra4-wifis3-peer-failure-recovery-gate" /
          "ra4-wifis3-peer-failure-recovery-gate.ino").read_text(encoding="ascii")
RUNNER = ROOT / "tools" / "run_ra4_wifis3_peer_failure_recovery_gate.py"


def require(source: str, text: str) -> None:
    if text not in source:
        raise RuntimeError(f"peer failure gate missing: {text}")


for required in (
    "delay(1200);",
    "RA4_PEER_FAILURE:TCP_RST_ERROR_PASS",
    "RA4_PEER_FAILURE:TCP_RECOVERY_PASS",
    "constexpr unsigned long kUdpDropTimeoutMs = 2500;",
    "RA4_PEER_FAILURE:UDP_TIMEOUT_PASS",
    "RA4_PEER_FAILURE:UDP_RECOVERY_PASS",
    "RA4_PEER_FAILURE:RESULT=PASS",
):
    require(SKETCH, required)
if not RUNNER.is_file():
    raise RuntimeError("peer failure runner is missing")
runner = RUNNER.read_text(encoding="ascii")
for required in (
    "PEER_FAILURE:TCP_RST_RECEIVED",
    "PEER_FAILURE:UDP_DROP_RECEIVED",
    "prepare_wifis3_result_error_overlay.py",
    '"--library", str(overlay_library)',
    "RA4_WIFIS3_PEER_FAILURE_RECOVERY_GATE_PASS",
    "RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_PASS",
):
    require(runner, required)
print("RA4_WIFIS3_PEER_FAILURE_RECOVERY_SOURCE_PASS")
