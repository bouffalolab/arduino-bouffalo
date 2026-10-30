#!/usr/bin/env python3
"""Keep the RA4 WiFiS3 TCP resource gate on real multi-client APIs."""

from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parents[1]
SKETCH = (
    ROOT_DIR
    / "examples"
    / "ra4-wifis3-tcp-resource-gate"
    / "ra4-wifis3-tcp-resource-gate.ino"
)
RUNNER = ROOT_DIR / "tools" / "run_ra4_wifis3_tcp_resource_gate.py"


def require(source: str, text: str, message: str) -> None:
    if text not in source:
        raise RuntimeError(message)


def main() -> int:
    sketch = SKETCH.read_text(encoding="ascii")
    runner = RUNNER.read_text(encoding="ascii")
    require(
        sketch,
        "constexpr size_t kConcurrentClientCount = 4U;",
        "TCP resource gate must retain four concurrent WiFiClient objects",
    )
    require(
        sketch,
        "clients[kMiddleClientIndex].stop();",
        "TCP resource gate must close a middle client slot",
    )
    require(
        sketch,
        "RA4_TCP_RESOURCE:MIDDLE_REUSE_PASS",
        "TCP resource gate must prove a released client slot is reusable",
    )
    require(
        sketch,
        "refused_client.connect(peer, static_cast<uint16_t>(port + 1U)) != 0",
        "TCP resource gate must exercise an actual refused or timed-out connection",
    )
    require(
        sketch,
        "RA4_TCP_RESOURCE:REFUSED_RELEASE_PASS",
        "TCP resource gate must prove a failed connection slot is released",
    )
    require(
        sketch,
        "client.remoteIP() == peer",
        "TCP resource gate must verify remoteIP for every successful client",
    )
    require(
        sketch,
        "client.remotePort() == port",
        "TCP resource gate must verify remotePort for every successful client",
    )
    require(
        sketch,
        "client.connected() == 1U",
        "TCP resource gate must verify connected() for every successful client",
    )
    require(
        sketch,
        "RA4_TCP_RESOURCE:INITIAL_ECHO_PASS",
        "TCP resource gate must preserve application-data evidence",
    )
    require(
        sketch,
        "constexpr size_t kStressCycles = 3U;",
        "TCP resource gate must retain three bounded reconnect cycles",
    )
    require(
        sketch,
        "RA4_TCP_RESOURCE:CYCLE_PASS=",
        "TCP resource gate must report each reconnect cycle",
    )
    require(
        sketch,
        "RA4_TCP_RESOURCE:ELAPSED_MS=",
        "TCP resource gate must report bounded runtime",
    )
    require(
        runner,
        "EXPECTED_PEER_EXCHANGES = 6",
        "runner must retain six successful peer exchanges",
    )
    require(
        runner,
        '"TCP_RESOURCE_PEER:ECHO_PASS=" + str(expected_exchanges)',
        "runner must require the selected number of unique peer exchanges",
    )
    require(
        runner,
        "EXPECTED_PEER_EXCHANGES * (",
        "runner must scale peer exchanges for reconnect stress cycles",
    )
    require(
        runner,
        "RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_PASS",
        "runner must restore the standard final-carrier scan Sketch",
    )
    require(
        runner,
        "RA4_WIFIS3_TCP_STRESS_GATE_PASS",
        "runner must retain the TCP reconnect stress gate",
    )
    require(
        runner,
        "normalized_runtime.count(marker) != expected_cycles",
        "runner must require every repeated TCP cycle",
    )
    require(
        runner,
        "240.0 if args.expected_exchanges > 6 else 90.0",
        "Windows TCP peer must remain alive for reconnect stress",
    )
    require(
        runner,
        "240.0 if args.stress else 0.0",
        "runner must extend the serial gate timeout for reconnect stress",
    )
    require(
        runner,
        "load_private_credentials",
        "runner must keep STA credentials in the private process environment",
    )
    print("RA4_WIFIS3_TCP_RESOURCE_SOURCE_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
