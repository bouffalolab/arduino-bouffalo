#!/usr/bin/env python3
"""Diagnose final-carrier BL616CL STA reachability to the Windows WLAN peer.

The runner uses the private STA credentials only in its current process. It
temporarily replaces the RA4M1 Sketch, then restores the WiFiS3 scan Sketch on
every exit. It neither prints credentials nor writes them to the repository.
"""

from __future__ import annotations

import argparse
import ipaddress
import json
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
import run_ra4_wifis3_scan_final_carrier as final_scan
from uno_r4_usb_identity import resolve_paired_bl616cl_port


ROOT_DIR = Path(__file__).resolve().parents[1]
FQBN = "arduino:renesas_uno:unor4wifi"
SKETCH = ROOT_DIR / "examples" / "ra4-wifis3-sta-peer-diagnostic"
BUILD_DIR = ROOT_DIR / "build" / "ra4-wifis3-sta-peer-diagnostic"
IMAGE = BUILD_DIR / "ra4-wifis3-sta-peer-diagnostic.ino.bin"
SAMBA_RUNNER = ROOT_DIR / "tools" / "run_ra4_bossac_final_carrier.py"
WORK_ROOT = ROOT_DIR / "build" / "ra4-wifis3-sta-peer-diagnostic-peer"
DEFAULT_TCP_PORT = 18443
TCP_REQUEST = b"bl616cl-peer-ping"
TCP_REPLY = b"bl616cl-peer-echo"

WINDOWS_PEER = r"""
import argparse
import socket

REQUEST = b"bl616cl-peer-ping"
REPLY = b"bl616cl-peer-echo"


def emit(message):
    print(message, flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind-ip", required=True)
    parser.add_argument("--port", required=True, type=int)
    args = parser.parse_args()

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((args.bind_ip, args.port))
    listener.listen(1)
    listener.settimeout(30.0)
    emit("STA_PEER:READY")
    try:
        connection, _ = listener.accept()
        emit("STA_PEER:TCP_ACCEPTED")
        with connection:
            connection.settimeout(5.0)
            received = connection.recv(128)
            if received == REQUEST:
                connection.sendall(REPLY)
                emit("STA_PEER:ECHO_PASS")
            else:
                emit("STA_PEER:REQUEST_REJECTED")
    except socket.timeout:
        emit("STA_PEER:ACCEPT_TIMEOUT")
    finally:
        listener.close()


if __name__ == "__main__":
    main()
"""


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def encode_start_value(value: str) -> str:
    return value.encode("ascii", "strict").hex()


def wsl_unc_path(path: Path) -> str:
    return "\\\\wsl.localhost\\Ubuntu-22.04" + str(path.resolve())


def windows_wlan_address() -> ipaddress.IPv4Address:
    command = (
        '$ErrorActionPreference = "Stop"; '
        'Get-NetIPConfiguration | '
        'Where-Object { $_.InterfaceAlias -eq "WLAN" -and $_.IPv4Address -and $_.IPv4DefaultGateway } | '
        'ForEach-Object { [PSCustomObject]@{ Address=$_.IPv4Address.IPAddress } } | '
        'ConvertTo-Json -Compress'
    )
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-Command", command],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if result.returncode != 0:
        raise RuntimeError("cannot query the Windows WLAN IPv4 address")
    try:
        data = json.loads(result.stdout)
        if isinstance(data, list):
            if len(data) != 1:
                raise ValueError()
            data = data[0]
        return ipaddress.IPv4Address(data["Address"])
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        raise RuntimeError("Windows WLAN does not have one usable IPv4 address") from error


def compile_sketch(args: argparse.Namespace) -> Path:
    run_command(
        [
            args.arduino_cli,
            "compile",
            "--fqbn",
            FQBN,
            "--build-path",
            str(BUILD_DIR),
            str(SKETCH),
        ]
    )
    if not IMAGE.is_file():
        raise RuntimeError("compiled RA4M1 peer diagnostic image does not exist")
    return IMAGE


def upload_sketch(args: argparse.Namespace, image: Path) -> None:
    run_command(
        [
            sys.executable,
            str(SAMBA_RUNNER),
            "--bridge-port",
            args.bridge_port,
            "--image",
            str(image),
            "--runtime-settle-ms",
            str(args.runtime_settle_ms),
            "--attempts",
            "3",
        ]
    )


def restore_scan(args: argparse.Namespace) -> None:
    run_command(
        [
            sys.executable,
            str(final_scan.__file__),
            "--bridge-port",
            args.bridge_port,
            "--arduino-cli",
            args.arduino_cli,
        ]
    )
    print("RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_PASS")


def wait_for_markers(
    bridge: serial.Serial,
    markers: tuple[str, ...],
    timeout_seconds: float,
) -> str:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        payload = bridge.read(512)
        if not payload:
            continue
        received.extend(payload)
        if len(received) > 8192:
            raise RuntimeError("RA4M1 diagnostic output exceeded its byte limit")
        text = received.decode("ascii", errors="replace").replace("\x00", "")
        if any(marker in text for marker in markers):
            return text
    raise RuntimeError("RA4M1 diagnostic did not produce the expected marker")


def print_markers(label: str, text: str, prefix: str) -> None:
    markers = [
        line.strip()
        for line in text.replace("\x00", "").splitlines()
        if line.strip().startswith(prefix)
    ]
    if markers:
        print(label + "_BEGIN")
        for marker in markers:
            print(marker)
        print(label + "_END")


def launch_windows_peer(
    work_dir: Path,
    peer: ipaddress.IPv4Address,
    port: int,
) -> subprocess.Popen[str]:
    script = work_dir / "windows_sta_peer.py"
    script.write_text(textwrap.dedent(WINDOWS_PEER), encoding="ascii")
    return subprocess.Popen(
        [
            "powershell.exe",
            "-NoProfile",
            "-Command",
            (
                f'& py.exe -3 "{wsl_unc_path(script)}" '
                f"--bind-ip {peer} --port {port}"
            ),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def wait_for_peer_ready(peer: subprocess.Popen[str], timeout_seconds: float) -> str:
    assert peer.stdout is not None
    deadline = time.monotonic() + timeout_seconds
    captured = ""
    while time.monotonic() < deadline:
        if peer.poll() is not None:
            captured += peer.stdout.read()
            break
        line = peer.stdout.readline()
        if line:
            captured += line
            if "STA_PEER:READY" in captured:
                return captured
    raise RuntimeError("Windows TCP peer did not become ready")


def stop_windows_peer(peer: subprocess.Popen[str]) -> str:
    if peer.poll() is None:
        try:
            peer.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            subprocess.run(
                ["taskkill.exe", "/PID", str(peer.pid), "/T", "/F"],
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
    if peer.stdout is None:
        return ""
    return peer.stdout.read()


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--credential-env", type=Path, default=DEFAULT_CREDENTIAL_ENV)
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--tcp-port", type=int, default=DEFAULT_TCP_PORT)
    parser.add_argument("--ready-timeout-ms", type=int, default=15000)
    parser.add_argument("--diagnostic-timeout-ms", type=int, default=60000)
    parser.add_argument("--runtime-settle-ms", type=int, default=1000)
    parser.add_argument(
        "--source-only",
        action="store_true",
        help="Compile the RA4M1 diagnostic without using hardware.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if (
        args.tcp_port <= 0
        or args.tcp_port > 65535
        or args.ready_timeout_ms <= 0
        or args.diagnostic_timeout_ms <= 0
        or args.runtime_settle_ms < 0
    ):
        raise RuntimeError("invalid port or timeout")
    if args.source_only:
        compile_sketch(args)
        print("RA4_WIFIS3_STA_PEER_DIAGNOSTIC_SOURCE_PASS")
        return 0

    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier_bridge(args.bridge_port)
    ssid, password = load_private_credentials(args.credential_env)
    if password is None:
        raise RuntimeError("STA peer diagnostic requires a private WPA password")
    peer_address = windows_wlan_address()

    print(f"Bridge: {args.bridge_port} (final-carrier BL616CL 2341:1002)")
    print("Target: RA4M1 application Flash only; BL616CL Flash is untouched.")
    print("Privacy: STA credentials and WLAN addresses remain in process memory.")

    peer = None
    bridge = None
    ra4_modified = False
    primary_error: Exception | None = None
    peer_output = ""
    try:
        WORK_ROOT.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="peer-", dir=WORK_ROOT) as work:
            image = compile_sketch(args)
            peer = launch_windows_peer(Path(work), peer_address, args.tcp_port)
            peer_output = wait_for_peer_ready(
                peer, args.ready_timeout_ms / 1000.0
            )
            upload_sketch(args, image)
            ra4_modified = True

            bridge = serial.Serial(
                args.bridge_port,
                baudrate=115200,
                timeout=0.1,
                write_timeout=1.0,
            )
            bridge.dtr = False
            bridge.rts = False
            bridge.reset_input_buffer()
            startup = wait_for_markers(
                bridge,
                ("RA4_STA_PEER:READY",),
                args.ready_timeout_ms / 1000.0,
            )
            print_markers("RA4_STA_PEER_STARTUP", startup, "RA4_STA_PEER:")
            bridge.reset_input_buffer()
            command = (
                "START "
                + encode_start_value(ssid)
                + " "
                + encode_start_value(password)
                + " "
                + str(peer_address)
                + " "
                + str(args.tcp_port)
                + "\n"
            )
            bridge.write(command.encode("ascii"))
            bridge.flush()
            runtime = wait_for_markers(
                bridge,
                ("RA4_STA_PEER:RESULT=PASS", "RA4_STA_PEER:RESULT=FAIL:"),
                args.diagnostic_timeout_ms / 1000.0,
            )
            print_markers("RA4_STA_PEER_RUNTIME", runtime, "RA4_STA_PEER:")
            if "RA4_STA_PEER:RESULT=PASS" not in runtime:
                raise RuntimeError("RA4M1 STA peer diagnostic reported failure")
            try:
                peer.wait(timeout=5.0)
            except subprocess.TimeoutExpired as error:
                raise RuntimeError(
                    "Windows TCP peer did not observe the completed RA4M1 exchange"
                ) from error
            peer_output += peer.stdout.read() if peer.stdout is not None else ""
            if (
                "STA_PEER:TCP_ACCEPTED" not in peer_output
                or "STA_PEER:ECHO_PASS" not in peer_output
            ):
                raise RuntimeError(
                    "Windows TCP peer did not confirm the expected RA4M1 exchange"
                )
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as error:
        primary_error = error
    finally:
        if bridge is not None and bridge.is_open:
            bridge.close()
        if peer is not None:
            peer_output += stop_windows_peer(peer)
        print_markers("STA_PEER_RUNTIME", peer_output, "STA_PEER:")
        if ra4_modified:
            try:
                restore_scan(args)
            except (
                OSError,
                RuntimeError,
                serial.SerialException,
                subprocess.SubprocessError,
            ):
                print("RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_FAILED", file=sys.stderr)
                if primary_error is None:
                    primary_error = RuntimeError("RA4M1 scan restore failed")

    if primary_error is not None:
        raise primary_error
    print("RA4_WIFIS3_STA_PEER_DIAGNOSTIC_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
