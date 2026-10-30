#!/usr/bin/env python3
"""Keep the network throughput gate bounded and payload-validating."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SKETCH = (ROOT / "examples" / "ra4-wifis3-network-throughput-gate" /
          "ra4-wifis3-network-throughput-gate.ino").read_text(encoding="ascii")
RUNNER_PATH = ROOT / "tools" / "run_ra4_wifis3_network_throughput_gate.py"


def require(source: str, text: str) -> None:
    if text not in source:
        raise RuntimeError(f"network throughput gate missing: {text}")


for required in (
    "constexpr size_t kPayloadBytes = 1000;",
    "constexpr size_t kTcpChunks = 64;",
    "constexpr size_t kUdpPackets = 32;",
    "memcmp(receive_buffer, transmit_buffer",
    "udp.remoteIP() != peer",
    "udp.remotePort() != port",
    "RA4_NETWORK_THROUGHPUT:TCP_BYTES=",
    "RA4_NETWORK_THROUGHPUT:UDP_BYTES=",
    "RA4_NETWORK_THROUGHPUT:RESULT=PASS",
):
    require(SKETCH, required)
if not RUNNER_PATH.is_file():
    raise RuntimeError("network throughput runner is missing")
runner = RUNNER_PATH.read_text(encoding="ascii")
for required in (
    "NETWORK_PEER:TCP_BYTES=64000",
    "NETWORK_PEER:UDP_BYTES=32000",
    "RA4_WIFIS3_NETWORK_THROUGHPUT_GATE_PASS",
    "RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_PASS",
    "load_private_credentials",
    "require_final_carrier_bridge",
):
    require(runner, required)
print("RA4_WIFIS3_NETWORK_THROUGHPUT_SOURCE_PASS")

# A real occupied TCP port must fail startup promptly rather than leave the
# main thread waiting forever at the startup barrier.
import importlib.util
import socket
import subprocess
import tempfile
import sys
spec = importlib.util.spec_from_file_location("network_gate", RUNNER_PATH)
module = importlib.util.module_from_spec(spec)
sys.path.insert(0, str(ROOT / "tools"))
spec.loader.exec_module(module)
with tempfile.TemporaryDirectory() as directory, socket.socket() as reserved:
    reserved.bind(("127.0.0.1", 0))
    reserved.listen(1)
    port = reserved.getsockname()[1]
    events = Path(directory) / "events"
    result = subprocess.run([sys.executable, "-c", module.PEER,
        "--bind-ip", "127.0.0.1", "--tcp-port", str(port),
        "--udp-port", "0", "--event-log", str(events)],
        timeout=5, capture_output=True)
    assert result.returncode != 0
    captured = events.read_text()
    assert "NETWORK_PEER:WORKER_ERROR=" in captured
    assert "NETWORK_PEER:READY" not in captured
    try:
        module.wait_event(events, "NETWORK_PEER:READY", 1)
    except RuntimeError as error:
        assert "NETWORK_PEER:WORKER_ERROR=" in str(error)
    else:
        raise AssertionError("bind failure was hidden")
print("RA4_NETWORK_PEER_BIND_FAILURE_PASS")
