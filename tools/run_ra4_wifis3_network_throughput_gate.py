#!/usr/bin/env python3
"""Run bounded TCP and UDP payload throughput on the final BL616CL carrier."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_bl616cl_apsta_gate import DEFAULT_CREDENTIAL_ENV, load_private_credentials
from run_ra4_bossac_final_carrier import require_final_carrier_bridge
from run_ra4_wifis3_tcp_resource_gate import (
    encode_start_value,
    print_markers,
    read_peer_event_log,
    run_command,
    stop_windows_peer,
    windows_wlan_address,
    wsl_unc_path,
)
import run_ra4_wifis3_scan_final_carrier as final_scan
from uno_r4_usb_identity import resolve_paired_bl616cl_port


ROOT = Path(__file__).resolve().parents[1]
SKETCH = ROOT / "examples" / "ra4-wifis3-network-throughput-gate"
BUILD = ROOT / "build" / "ra4-wifis3-network-throughput-gate"
IMAGE = BUILD / "ra4-wifis3-network-throughput-gate.ino.bin"
WORK = ROOT / "build" / "ra4-wifis3-network-throughput-peer"
TCP_BYTES = 64000
UDP_BYTES = 32000
PAYLOAD_BYTES = 1000

PEER = r"""
import argparse
import socket
import threading
import time

parser = argparse.ArgumentParser()
parser.add_argument("--bind-ip", required=True)
parser.add_argument("--tcp-port", type=int, required=True)
parser.add_argument("--udp-port", type=int, required=True)
parser.add_argument("--event-log", required=True)
args = parser.parse_args()
lock = threading.Lock()
ready = threading.Barrier(3)
results = {"tcp": False, "udp": False}


def emit(message):
    with lock:
        with open(args.event_log, "a", encoding="ascii") as events:
            events.write(message + "\n")


def valid_payload(payload, sequence):
    if len(payload) != 1000:
        return False
    if int.from_bytes(payload[:4], "big") != sequence:
        return False
    return all(
        value == ((sequence * 31 + index * 17) & 0xFF)
        for index, value in enumerate(payload[4:], start=4)
    )


def recv_exact(connection, size):
    data = bytearray()
    while len(data) < size:
        part = connection.recv(size - len(data))
        if not part:
            raise RuntimeError("TCP peer closed before a complete chunk")
        data.extend(part)
    return bytes(data)


def tcp_worker():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.bind_ip, args.tcp_port))
        listener.listen(1)
        listener.settimeout(300)
        ready.wait()
        connection, _ = listener.accept()
        with connection:
            connection.settimeout(20)
            for sequence in range(64):
                payload = recv_exact(connection, 1000)
                if not valid_payload(payload, sequence):
                    emit("NETWORK_PEER:TCP_PAYLOAD_FAIL")
                    return
                connection.sendall(payload)
        results["tcp"] = True
        emit("NETWORK_PEER:TCP_BYTES=64000")


def udp_worker():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
        peer.bind((args.bind_ip, args.udp_port))
        peer.settimeout(300)
        ready.wait()
        for packet in range(32):
            payload, source = peer.recvfrom(1200)
            sequence = 0x80000000 + packet
            if not valid_payload(payload, sequence):
                emit("NETWORK_PEER:UDP_PAYLOAD_FAIL")
                return
            peer.sendto(payload, source)
        results["udp"] = True
        emit("NETWORK_PEER:UDP_BYTES=32000")


def guarded_worker(worker):
    try:
        worker()
    except Exception as error:
        # Numeric errors identify failed binds without logging peer addresses.
        emit("NETWORK_PEER:WORKER_ERROR=%s,%s,%s" % (
            type(error).__name__, getattr(error, "errno", None),
            getattr(error, "winerror", None)))
        ready.abort()


threads = [
    threading.Thread(target=guarded_worker, args=(tcp_worker,), daemon=True),
    threading.Thread(target=guarded_worker, args=(udp_worker,), daemon=True),
]
for thread in threads:
    thread.start()
try:
    ready.wait(timeout=10)
except threading.BrokenBarrierError:
    emit("NETWORK_PEER:STARTUP_FAILED")
    raise SystemExit(1)
emit("NETWORK_PEER:READY")
for thread in threads:
    thread.join()
if results["tcp"] and results["udp"]:
    emit("NETWORK_PEER:RESULT=PASS")
"""


def compile_sketch(cli: str) -> None:
    run_command([
        cli, "compile", "--clean", "--fqbn",
        "arduino:renesas_uno:unor4wifi", "--build-path", str(BUILD), str(SKETCH),
    ])
    if not IMAGE.is_file():
        raise RuntimeError("compiled network throughput image is missing")


def configure_firewall(address: object, tcp_port: int, udp_port: int) -> list[str]:
    base = f"BL616CL-NETWORK-THROUGHPUT-{os.getpid()}"
    rules = [base + "-TCP", base + "-UDP"]
    command = (
        '$ErrorActionPreference = "Stop"; '
        f'New-NetFirewallRule -DisplayName "{rules[0]}" -Direction Inbound '
        f'-Action Allow -Profile Any -Protocol TCP -LocalAddress {address} '
        f'-LocalPort {tcp_port} | Out-Null; '
        f'New-NetFirewallRule -DisplayName "{rules[1]}" -Direction Inbound '
        f'-Action Allow -Profile Any -Protocol UDP -LocalAddress {address} '
        f'-LocalPort {udp_port} | Out-Null; Write-Output "FIREWALL_READY"'
    )
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-Command", command],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
        check=False,
    )
    if result.returncode != 0 or "FIREWALL_READY" not in result.stdout:
        remove_firewall(rules)
        raise RuntimeError("cannot create temporary Windows throughput firewall rules")
    print("NETWORK_PEER:FIREWALL_READY")
    return rules


def remove_firewall(rules: list[str]) -> None:
    for rule in rules:
        subprocess.run(
            ["powershell.exe", "-NoProfile", "-Command",
             f'Remove-NetFirewallRule -DisplayName "{rule}" '
             "-ErrorAction SilentlyContinue"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False,
        )


def wait_event(path: Path, marker: str, timeout: float) -> str:
    deadline = time.monotonic() + timeout
    observed = ""
    while time.monotonic() < deadline:
        observed = read_peer_event_log(path)
        errors = [line for line in observed.splitlines()
                  if line.startswith(("NETWORK_PEER:WORKER_ERROR=", "NETWORK_PEER:STARTUP_FAILED"))]
        if errors:
            raise RuntimeError("; ".join(errors))
        if marker in observed:
            return observed
        time.sleep(0.05)
    raise RuntimeError(f"Windows throughput peer did not emit {marker}")


def wait_ready(bridge: serial.Serial, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    received = bytearray()
    while time.monotonic() < deadline:
        received.extend(bridge.read(256))
        if b"RA4_NETWORK_THROUGHPUT:READY" in received:
            return
        if len(received) > 4096:
            del received[:-1024]
    raise RuntimeError("RA4M1 network throughput Sketch did not start")


def capture_result(bridge: serial.Serial, timeout: float) -> str:
    deadline = time.monotonic() + timeout
    received = bytearray()
    while time.monotonic() < deadline:
        received.extend(bridge.read(512))
        if len(received) > 16384:
            raise RuntimeError("RA4M1 throughput output exceeded limit")
        text = received.decode("ascii", errors="replace").replace(
            "\x00", "").replace("\r", "")
        if "RA4_NETWORK_THROUGHPUT:RESULT=FAIL:" in text:
            print_markers("RA4_NETWORK_THROUGHPUT_DIAGNOSTICS", text,
                          "RA4_NETWORK_THROUGHPUT:")
            raise RuntimeError("RA4M1 network throughput gate failed")
        if "RA4_NETWORK_THROUGHPUT:RESULT=PASS" in text:
            for marker in (
                "RA4_NETWORK_THROUGHPUT:TCP_BYTES=64000",
                "RA4_NETWORK_THROUGHPUT:TCP_CHUNKS=64",
                "RA4_NETWORK_THROUGHPUT:UDP_BYTES=32000",
                "RA4_NETWORK_THROUGHPUT:UDP_PACKETS=32",
            ):
                if marker not in text:
                    raise RuntimeError("RA4M1 throughput result is incomplete")
            return text
    raise RuntimeError("RA4M1 network throughput gate timed out")


def marker_number(text: str, name: str) -> int:
    match = re.search(rf"^{re.escape(name)}=(\d+)$", text, re.MULTILINE)
    if match is None:
        raise RuntimeError(f"missing numeric marker {name}")
    value = int(match.group(1))
    if value <= 0:
        raise RuntimeError(f"invalid numeric marker {name}")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--credential-env", type=Path, default=DEFAULT_CREDENTIAL_ENV)
    parser.add_argument("--tcp-port", type=int, default=18443)
    parser.add_argument("--udp-port", type=int, default=18448)
    parser.add_argument("--skip-peer-firewall", action="store_true")
    parser.add_argument("--source-only", action="store_true")
    args = parser.parse_args()
    if not (1 <= args.tcp_port <= 65535 and 1 <= args.udp_port <= 65535):
        raise RuntimeError("invalid throughput peer port")
    if args.source_only:
        compile_sketch(args.arduino_cli)
        print("RA4_WIFIS3_NETWORK_THROUGHPUT_SOURCE_COMPILE_PASS")
        return 0

    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier_bridge(args.bridge_port)
    ssid, password = load_private_credentials(args.credential_env)
    if password is None:
        raise RuntimeError("network throughput gate requires a private WPA password")
    address = windows_wlan_address()
    print(f"Bridge: {args.bridge_port} (verified BL616CL final carrier)")
    print("Target: temporary RA4M1 Sketch; BL616CL Flash unchanged.")
    print("Privacy: credentials and peer address remain in process memory.")

    WORK.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="network-throughput-", dir=WORK))
    events = directory / "peer.events"
    peer = None
    bridge = None
    firewall_rules: list[str] = []
    ra4_modified = False
    error: Exception | None = None
    try:
        compile_sketch(args.arduino_cli)
        if args.skip_peer_firewall:
            print("NETWORK_PEER:FIREWALL_SKIPPED")
        else:
            firewall_rules = configure_firewall(
                address, args.tcp_port, args.udp_port)
        peer_script = directory / "peer.py"
        peer_script.write_text(textwrap.dedent(PEER), encoding="ascii")
        peer = subprocess.Popen(
            ["powershell.exe", "-NoProfile", "-Command",
             f'& py.exe -3 "{wsl_unc_path(peer_script)}" --bind-ip {address} '
             f'--tcp-port {args.tcp_port} --udp-port {args.udp_port} '
             f'--event-log "{wsl_unc_path(events)}"'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        wait_event(events, "NETWORK_PEER:READY", 15)
        ra4_modified = True
        run_command([
            sys.executable,
            str(ROOT / "tools" / "run_ra4_bossac_final_carrier.py"),
            "--bridge-port", args.bridge_port, "--image", str(IMAGE),
            "--runtime-settle-ms", "1000", "--attempts", "3",
        ])
        bridge = serial.Serial(
            args.bridge_port, baudrate=115200, timeout=0.1, write_timeout=1)
        bridge.dtr = False
        bridge.rts = False
        bridge.reset_input_buffer()
        wait_ready(bridge, 15)
        bridge.reset_input_buffer()
        bridge.write(
            f"START {encode_start_value(ssid)} {encode_start_value(password)} "
            f"{address} {args.tcp_port} {args.udp_port}\n".encode("ascii")
        )
        bridge.flush()
        output = capture_result(bridge, 600)
        print_markers("RA4_NETWORK_THROUGHPUT_RUNTIME", output,
                      "RA4_NETWORK_THROUGHPUT:")
        peer_output = wait_event(events, "NETWORK_PEER:RESULT=PASS", 10)
        for marker in (
            "NETWORK_PEER:TCP_BYTES=64000",
            "NETWORK_PEER:UDP_BYTES=32000",
        ):
            if marker not in peer_output:
                raise RuntimeError("Windows throughput peer result is incomplete")
            print(marker)
        tcp_ms = marker_number(
            output, "RA4_NETWORK_THROUGHPUT:TCP_ELAPSED_MS")
        udp_ms = marker_number(
            output, "RA4_NETWORK_THROUGHPUT:UDP_ELAPSED_MS")
        print(f"RA4_NETWORK_THROUGHPUT:TCP_ONE_WAY_GOODPUT_BPS="
              f"{TCP_BYTES * 1000 // tcp_ms}")
        print(f"RA4_NETWORK_THROUGHPUT:TCP_ROUNDTRIP_PAYLOAD_BPS="
              f"{TCP_BYTES * 2000 // tcp_ms}")
        print(f"RA4_NETWORK_THROUGHPUT:UDP_ONE_WAY_GOODPUT_BPS="
              f"{UDP_BYTES * 1000 // udp_ms}")
        print(f"RA4_NETWORK_THROUGHPUT:UDP_ROUNDTRIP_PAYLOAD_BPS="
              f"{UDP_BYTES * 2000 // udp_ms}")
        print("RA4_WIFIS3_NETWORK_THROUGHPUT_GATE_PASS")
    except (OSError, RuntimeError, serial.SerialException,
            subprocess.SubprocessError) as caught:
        error = caught
    finally:
        if bridge is not None and bridge.is_open:
            bridge.close()
        if peer is not None:
            stop_windows_peer(peer, directory)
        remove_firewall(firewall_rules)
        if ra4_modified:
            try:
                run_command([
                    sys.executable, str(final_scan.__file__),
                    "--bridge-port", args.bridge_port,
                    "--arduino-cli", args.arduino_cli,
                ])
                print("RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_PASS")
            except (OSError, RuntimeError, subprocess.SubprocessError):
                print("RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_FAILED",
                      file=sys.stderr)
                if error is None:
                    error = RuntimeError("RA4M1 scan restore failed")
        shutil.rmtree(directory, ignore_errors=True)
    if error is not None:
        raise error
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException,
            subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
