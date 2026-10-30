#!/usr/bin/env python3
"""Keep the UDP resource gate tied to real datagrams and scan restoration."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SKETCH = (ROOT / "examples" / "ra4-wifis3-udp-resource-gate" /
          "ra4-wifis3-udp-resource-gate.ino").read_text(encoding="ascii")
RUNNER = (ROOT / "tools" / "run_ra4_wifis3_udp_resource_gate.py").read_text(
    encoding="ascii"
)


def require(source: str, text: str) -> None:
    if text not in source:
        raise RuntimeError(f"UDP resource gate missing required behavior: {text}")


for required in (
    "constexpr size_t kSlots = 4;",
    "constexpr size_t kRounds = 3;",
    "extra.begin(0) != 0",
    "extra.begin(kBasePort + kSlots) != 0",
    "sockets[kMiddle].stop();",
    "reused.begin(kBasePort + kMiddle)",
    "udp.parsePacket()",
    "udp.remoteIP() != peer",
    "udp.remotePort() != port",
    "udp.read(reinterpret_cast<unsigned char *>(reply), length)",
    "RA4_UDP_RESOURCE:RESULT=PASS",
    "constexpr size_t kStressCycles = 3;",
    "cycle * (kRounds + 1)",
    "WiFi.status() == WL_CONNECTED",
    "RA4_UDP_RESOURCE:CYCLE_PASS=",
    "RA4_UDP_RESOURCE:ELAPSED_MS=",
):
    require(SKETCH, required)
for required in (
    "EXCHANGES = 13",
    "UDP_RESOURCE_PEER:ECHO_PASS=",
    "ra4_modified = True",
    "RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_PASS",
    "load_private_credentials",
    "require_final_carrier_bridge",
    "RA4_WIFIS3_UDP_STRESS_GATE_PASS",
    "f'--cycles {3 if args.stress else 1} --large-datagrams {args.large_datagrams}'",
    "text.count(\"RA4_UDP_RESOURCE:REBIND_PASS\") != cycles",
):
    require(RUNNER, required)
print("RA4_WIFIS3_UDP_RESOURCE_SOURCE_PASS")
