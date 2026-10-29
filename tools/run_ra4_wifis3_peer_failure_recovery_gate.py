#!/usr/bin/env python3
"""Run controlled TCP-RST and UDP-drop recovery on the final BL616CL carrier."""

from __future__ import annotations

import argparse
import os
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
SKETCH = ROOT / "examples" / "ra4-wifis3-peer-failure-recovery-gate"
BUILD = ROOT / "build" / "ra4-wifis3-peer-failure-recovery-gate"
IMAGE = BUILD / "ra4-wifis3-peer-failure-recovery-gate.ino.bin"
WORK = ROOT / "build" / "ra4-wifis3-peer-failure-recovery-peer"
OVERLAY_PREPARER = ROOT / "tools" / "prepare_wifis3_result_error_overlay.py"
TCP_PORT = 18449
UDP_PORT = 18450

PEER = r"""
import argparse
import socket
import struct
import threading

parser = argparse.ArgumentParser()
parser.add_argument("--bind-ip", required=True)
parser.add_argument("--tcp-port", type=int, required=True)
parser.add_argument("--udp-port", type=int, required=True)
parser.add_argument("--event-log", required=True)
args = parser.parse_args()
lock = threading.Lock()


def emit(message):
    with lock:
        with open(args.event_log, "a", encoding="ascii") as events:
            events.write(message + "\n")


def recv_exact(connection, size):
    data = bytearray()
    while len(data) < size:
        part = connection.recv(size - len(data))
        if not part:
            return bytes(data)
        data.extend(part)
    return bytes(data)


def tcp_worker():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.bind_ip, args.tcp_port))
        listener.listen(2)
        listener.settimeout(120)
        first, _ = listener.accept()
        with first:
            linger = struct.pack("ii", 1, 0)
            first.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, linger)
            emit("PEER_FAILURE:TCP_RST_RECEIVED")
        second, _ = listener.accept()
        with second:
            second.settimeout(20)
            payload = recv_exact(second, 32)
            if payload != bytes((0x30 + index) & 0xFF for index in range(32)):
                emit("PEER_FAILURE:TCP_RECOVERY_PAYLOAD_FAIL")
                return
            second.sendall(payload)
            emit("PEER_FAILURE:TCP_RECOVERY_ECHO_PASS")


def udp_worker():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
        peer.bind((args.bind_ip, args.udp_port))
        peer.settimeout(120)
        first, source = peer.recvfrom(128)
        if len(first) != 32:
            emit("PEER_FAILURE:UDP_DROP_PAYLOAD_FAIL")
            return
        emit("PEER_FAILURE:UDP_DROP_RECEIVED")
        second, source = peer.recvfrom(128)
        if second != first:
            emit("PEER_FAILURE:UDP_RECOVERY_PAYLOAD_FAIL")
            return
        peer.sendto(second, source)
        emit("PEER_FAILURE:UDP_RECOVERY_ECHO_PASS")


threads = [
    threading.Thread(target=tcp_worker),
    threading.Thread(target=udp_worker),
]
for thread in threads:
    thread.start()
emit("PEER_FAILURE:READY")
for thread in threads:
    thread.join()
emit("PEER_FAILURE:RESULT=PASS")
"""


def compile_sketch(cli: str, work_dir: Path) -> None:
    overlay_library = work_dir / "libraries" / "WiFiS3"
    run_command([
        sys.executable,
        str(OVERLAY_PREPARER),
        "--output-library",
        str(overlay_library),
    ])
    run_command([
        cli, "compile", "--clean", "--fqbn",
        "arduino:renesas_uno:unor4wifi", "--build-path", str(BUILD),
        "--library", str(overlay_library), str(SKETCH),
    ])
    if not IMAGE.is_file():
        raise RuntimeError("peer failure image is missing")


def wait_event(path: Path, marker: str, timeout: float) -> str:
    deadline = time.monotonic() + timeout
    observed = ""
    while time.monotonic() < deadline:
        observed = read_peer_event_log(path)
        if marker in observed:
            return observed
        time.sleep(0.05)
    raise RuntimeError(f"peer did not emit {marker}")


def wait_ready(bridge: serial.Serial, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    received = bytearray()
    while time.monotonic() < deadline:
        received.extend(bridge.read(256))
        if b"RA4_PEER_FAILURE:READY" in received:
            return
        if len(received) > 4096:
            del received[:-1024]
    raise RuntimeError("RA4M1 peer failure Sketch did not start")


def capture_result(bridge: serial.Serial, timeout: float) -> str:
    deadline = time.monotonic() + timeout
    received = bytearray()
    while time.monotonic() < deadline:
        received.extend(bridge.read(512))
        text = received.decode("ascii", errors="replace").replace(
            "\x00", "").replace("\r", "")
        if "RA4_PEER_FAILURE:RESULT=FAIL:" in text:
            print_markers("RA4_PEER_FAILURE_DIAGNOSTICS", text, "RA4_PEER_FAILURE:")
            raise RuntimeError("RA4M1 peer failure gate failed")
        if "RA4_PEER_FAILURE:RESULT=PASS" in text:
            for marker in (
                "RA4_PEER_FAILURE:TCP_RST_ERROR_PASS",
                "RA4_PEER_FAILURE:TCP_RECOVERY_PASS",
                "RA4_PEER_FAILURE:UDP_TIMEOUT_PASS",
                "RA4_PEER_FAILURE:UDP_RECOVERY_PASS",
            ):
                if marker not in text:
                    raise RuntimeError("peer failure result is incomplete")
            return text
    raise RuntimeError("RA4M1 peer failure gate timed out")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--credential-env", type=Path, default=DEFAULT_CREDENTIAL_ENV)
    parser.add_argument("--source-only", action="store_true")
    args = parser.parse_args()
    if args.source_only:
        source_work = Path(tempfile.mkdtemp(prefix="peer-failure-source-", dir=WORK))
        try:
            compile_sketch(args.arduino_cli, source_work)
        finally:
            shutil.rmtree(source_work, ignore_errors=True)
        print("RA4_WIFIS3_PEER_FAILURE_RECOVERY_SOURCE_COMPILE_PASS")
        return 0

    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier_bridge(args.bridge_port)
    ssid, password = load_private_credentials(args.credential_env)
    if password is None:
        raise RuntimeError("peer failure gate requires a private WPA password")
    address = windows_wlan_address()
    print(f"Bridge: {args.bridge_port} (verified BL616CL final carrier)")
    print("Target: temporary RA4M1 Sketch; BL616CL Flash unchanged.")
    WORK.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="peer-failure-", dir=WORK))
    events = directory / "peer.events"
    peer = None
    bridge = None
    ra4_modified = False
    error: Exception | None = None
    try:
        compile_sketch(args.arduino_cli, directory)
        peer_script = directory / "peer.py"
        peer_script.write_text(textwrap.dedent(PEER), encoding="ascii")
        peer = subprocess.Popen(
            ["powershell.exe", "-NoProfile", "-Command",
             f'& py.exe -3 "{wsl_unc_path(peer_script)}" --bind-ip {address} '
             f'--tcp-port {TCP_PORT} --udp-port {UDP_PORT} '
             f'--event-log "{wsl_unc_path(events)}"'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        wait_event(events, "PEER_FAILURE:READY", 15)
        ra4_modified = True
        run_command([
            sys.executable, str(ROOT / "tools" / "run_ra4_bossac_final_carrier.py"),
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
            f"{address} {TCP_PORT} {UDP_PORT}\n".encode("ascii")
        )
        bridge.flush()
        output = capture_result(bridge, 90)
        print_markers("RA4_PEER_FAILURE_RUNTIME", output, "RA4_PEER_FAILURE:")
        peer_output = wait_event(events, "PEER_FAILURE:RESULT=PASS", 10)
        for marker in (
            "PEER_FAILURE:TCP_RST_RECEIVED",
            "PEER_FAILURE:TCP_RECOVERY_ECHO_PASS",
            "PEER_FAILURE:UDP_DROP_RECEIVED",
            "PEER_FAILURE:UDP_RECOVERY_ECHO_PASS",
        ):
            if marker not in peer_output:
                raise RuntimeError(f"peer failure result missing {marker}")
            print(marker)
        print("RA4_WIFIS3_PEER_FAILURE_RECOVERY_GATE_PASS")
    except (OSError, RuntimeError, serial.SerialException,
            subprocess.SubprocessError) as caught:
        error = caught
    finally:
        if bridge is not None and bridge.is_open:
            bridge.close()
        if peer is not None:
            stop_windows_peer(peer, directory)
        peer_output = read_peer_event_log(events)
        print_markers("PEER_FAILURE_RUNTIME", peer_output, "PEER_FAILURE:")
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
