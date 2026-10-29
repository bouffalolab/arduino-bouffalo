#!/usr/bin/env python3
"""Check the TLS throughput gate's framing and peer validation contract."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / "examples/ra4-wifis3-tls-throughput-gate/ra4-wifis3-tls-throughput-gate.ino.in"
RUNNER = ROOT / "tools/run_ra4_wifis3_tls_throughput_gate.py"


def main() -> int:
    template = TEMPLATE.read_text(encoding="ascii")
    runner = RUNNER.read_text(encoding="ascii")
    for marker in (
        "RA4_TLS_THROUGHPUT:TLS_CONNECTED",
        "RA4_TLS_THROUGHPUT:CHUNKS=",
        "RA4_TLS_THROUGHPUT:BYTES_EACH_DIRECTION=",
        "RA4_TLS_THROUGHPUT:ELAPSED_MS=",
        "RA4_TLS_THROUGHPUT:RESULT=PASS",
        "valid_payload",
        "TLS_THROUGHPUT_PEER:RESULT=PASS",
    ):
        if marker not in template and marker not in runner:
            raise RuntimeError(f"TLS throughput gate must contain {marker}")
    for marker in ("--source-only", "RA4_WIFIS3_TLS_THROUGHPUT_SOURCE_COMPILE_PASS",
                   "RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_PASS"):
        if marker not in runner and marker not in template:
            raise RuntimeError(f"TLS throughput runner must contain {marker}")
    print("RA4_WIFIS3_TLS_THROUGHPUT_SOURCE_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
