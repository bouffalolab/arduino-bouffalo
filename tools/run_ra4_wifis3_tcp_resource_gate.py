#!/usr/bin/env python3
"""Run a multi-client TCP resource gate through the final BL616CL carrier.

The temporary RA4M1 Sketch establishes four concurrent WiFiClient sessions to
a Windows WLAN peer, verifies endpoint and payload behavior, closes a middle
slot and reuses it, then proves a refused connection can be released and
reused. STA credentials and the WLAN address remain in process memory only.
The standard WiFiS3 scan Sketch is restored on every exit path.
"""

from __future__ import annotations

import argparse
import ipaddress
import json
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
import run_ra4_wifis3_scan_final_carrier as final_scan
from uno_r4_usb_identity import resolve_paired_bl616cl_port


ROOT_DIR = Path(__file__).resolve().parents[1]
FQBN = "arduino:renesas_uno:unor4wifi"
SKETCH = ROOT_DIR / "examples" / "ra4-wifis3-tcp-resource-gate"
BUILD_DIR = ROOT_DIR / "build" / "ra4-wifis3-tcp-resource-gate"
IMAGE = BUILD_DIR / "ra4-wifis3-tcp-resource-gate.ino.bin"
SAMBA_RUNNER = ROOT_DIR / "tools" / "run_ra4_bossac_final_carrier.py"
WORK_ROOT = ROOT_DIR / "build" / "ra4-wifis3-tcp-resource-peer"
DEFAULT_TCP_PORT = 18447
EXPECTED_PEER_EXCHANGES = 6
STRESS_CYCLES = 3


WINDOWS_PEER = r"""
import argparse
import socket
import threading
import time

REPLY_PREFIX = b"bl616cl-tcp-resource-echo:"


def emit(message, event_log):
    print(message, flush=True)
    with open(event_log, "a", encoding="ascii") as output:
        output.write(message + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind-ip", required=True)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--event-log", required=True)
    parser.add_argument("--expected-exchanges", required=True, type=int)
    args = parser.parse_args()
    if args.expected_exchanges <= 0:
        raise RuntimeError("expected exchange count must be positive")

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((args.bind_ip, args.port))
    listener.listen(args.expected_exchanges)
    listener.settimeout(0.2)
    stop = threading.Event()
    completed = {"count": 0}
    completed_lock = threading.Lock()

    def handle(connection, index):
        with connection:
            try:
                connection.settimeout(5.0)
                request = connection.recv(128)
                if not request.startswith(b"bl616cl-tcp-resource-"):
                    emit(f"TCP_RESOURCE_PEER:REQUEST_REJECTED={index}", args.event_log)
                    return
                connection.sendall(REPLY_PREFIX + request)
                with completed_lock:
                    completed["count"] += 1
                    count = completed["count"]
                emit(f"TCP_RESOURCE_PEER:ECHO_PASS={count}", args.event_log)
            except OSError:
                emit(f"TCP_RESOURCE_PEER:EXCHANGE_ERROR={index}", args.event_log)

    emit("TCP_RESOURCE_PEER:READY", args.event_log)
    accepted = 0
    deadline = time.monotonic() + (
        240.0 if args.expected_exchanges > 6 else 90.0)
    try:
        while time.monotonic() < deadline:
            with completed_lock:
                if completed["count"] >= args.expected_exchanges:
                    break
            try:
                connection, _ = listener.accept()
            except socket.timeout:
                continue
            accepted += 1
            emit(f"TCP_RESOURCE_PEER:ACCEPT={accepted}", args.event_log)
            threading.Thread(target=handle, args=(connection, accepted), daemon=True).start()
    finally:
        listener.close()
        stop.set()


if __name__ == "__main__":
    main()
"""


class MarkerCaptureError(RuntimeError):
    def __init__(self, marker: str, captured: str) -> None:
        super().__init__(f"RA4M1 TCP resource gate failed: {marker}")
        self.captured = captured


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


def configure_peer_firewall(host: ipaddress.IPv4Address, port: int) -> str:
    rule_name = f"BL616CL-TCP-RESOURCE-{os.getpid()}"
    command = (
        '$ErrorActionPreference = "Stop"; '
        f'Remove-NetFirewallRule -DisplayName "{rule_name}" -ErrorAction SilentlyContinue; '
        f'New-NetFirewallRule -DisplayName "{rule_name}" -Direction Inbound -Action Allow '
        f'-Profile Any -Protocol TCP -LocalAddress {host} -LocalPort {port} | Out-Null; '
        'Write-Output "FIREWALL_READY"'
    )
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-Command", command],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if result.returncode != 0 or "FIREWALL_READY" not in result.stdout:
        raise RuntimeError("cannot create the temporary Windows TCP peer firewall rule")
    print("TCP_RESOURCE_PEER:FIREWALL_READY")
    return rule_name


def remove_peer_firewall(rule_name: str | None) -> None:
    if rule_name is None:
        return
    subprocess.run(
        [
            "powershell.exe",
            "-NoProfile",
            "-Command",
            (
                '$ErrorActionPreference = "SilentlyContinue"; '
                f'Remove-NetFirewallRule -DisplayName "{rule_name}"'
            ),
        ],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def compile_sketch(args: argparse.Namespace) -> Path:
    run_command(
        [
            args.arduino_cli,
            "compile",
            "--clean",
            "--fqbn",
            FQBN,
            "--build-path",
            str(BUILD_DIR),
            str(SKETCH),
        ]
    )
    if not IMAGE.is_file():
        raise RuntimeError("compiled RA4M1 TCP resource image does not exist")
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


def launch_windows_peer(
    work_dir: Path,
    peer: ipaddress.IPv4Address,
    port: int,
    event_log: Path,
    expected_exchanges: int,
) -> subprocess.Popen[str]:
    script = work_dir / "windows_tcp_resource_peer.py"
    script.write_text(textwrap.dedent(WINDOWS_PEER), encoding="ascii")
    return subprocess.Popen(
        [
            "powershell.exe",
            "-NoProfile",
            "-Command",
            (
                f'& py.exe -3 "{wsl_unc_path(script)}" '
                f"--bind-ip {peer} --port {port} "
                f'--event-log "{wsl_unc_path(event_log)}" '
                f"--expected-exchanges {expected_exchanges}"
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
            if "TCP_RESOURCE_PEER:READY" in captured:
                return captured
    raise RuntimeError("temporary Windows TCP resource peer did not become ready")


def read_peer_event_log(path: Path) -> str:
    if not path.is_file():
        return ""
    return path.read_text(encoding="ascii", errors="replace")


def wait_for_peer_events(
    event_log: Path,
    required: tuple[str, ...],
    timeout_seconds: float,
) -> str:
    deadline = time.monotonic() + timeout_seconds
    observed = ""
    while time.monotonic() < deadline:
        observed = read_peer_event_log(event_log)
        if all(marker in observed for marker in required):
            return observed
        time.sleep(0.05)
    missing = ", ".join(marker for marker in required if marker not in observed)
    raise RuntimeError(f"temporary TCP peer did not emit required events: {missing}")


def stop_windows_peer(peer: subprocess.Popen[str], work_dir: Path) -> None:
    work_dir_match = wsl_unc_path(work_dir).replace("'", "''")
    subprocess.run(
        [
            "powershell.exe",
            "-NoProfile",
            "-Command",
            (
                "$needle = '"
                + work_dir_match
                + "'; Get-CimInstance Win32_Process | "
                "Where-Object { $_.CommandLine -and "
                "$_.CommandLine.Contains($needle) } | "
                "ForEach-Object { taskkill.exe /PID $_.ProcessId /T /F "
                "| Out-Null }"
            ),
        ],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if peer.poll() is None:
        try:
            peer.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            peer.kill()
            peer.wait(timeout=5.0)


def capture_until(
    bridge: serial.Serial,
    required: tuple[str, ...],
    timeout_seconds: float,
) -> str:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        payload = bridge.read(512)
        if not payload:
            continue
        received.extend(payload)
        if len(received) > 16384:
            raise RuntimeError("RA4M1 TCP resource output exceeded its byte limit")
        text = received.decode("ascii", errors="replace").replace("\x00", "")
        failure = "RA4_TCP_RESOURCE:RESULT=FAIL:"
        if failure in text:
            start = text.find(failure)
            end = text.find("\n", start)
            raise MarkerCaptureError(
                text[start:end if end >= 0 else len(text)].strip(),
                text,
            )
        if all(marker in text for marker in required):
            return text
    raise MarkerCaptureError("RA4_TCP_RESOURCE:MARKERS_TIMEOUT", received.decode(
        "ascii", errors="replace"
    ).replace("\x00", ""))


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


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--credential-env", type=Path, default=DEFAULT_CREDENTIAL_ENV)
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--tcp-port", type=int, default=DEFAULT_TCP_PORT)
    parser.add_argument("--runtime-settle-ms", type=int, default=1000)
    parser.add_argument("--ready-timeout-ms", type=int, default=15000)
    parser.add_argument("--gate-timeout-ms", type=int, default=90000)
    parser.add_argument("--skip-peer-firewall", action="store_true")
    parser.add_argument("--stress", action="store_true",
                        help="run three bounded STA/client reconnect cycles")
    parser.add_argument("--source-only", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if (
        args.tcp_port <= 0
        or args.tcp_port >= 65535
        or args.runtime_settle_ms < 0
        or args.ready_timeout_ms <= 0
        or args.gate_timeout_ms <= 0
    ):
        raise RuntimeError("invalid port or timeout")

    if args.source_only:
        compile_sketch(args)
        print("RA4_WIFIS3_TCP_RESOURCE_SOURCE_COMPILE_PASS")
        return 0

    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier_bridge(args.bridge_port)
    ssid, password = load_private_credentials(args.credential_env)
    if password is None:
        raise RuntimeError("TCP resource gate requires a private WPA password")
    peer_address = windows_wlan_address()

    print(f"Bridge: {args.bridge_port} (final-carrier BL616CL 2341:1002)")
    print("Target: RA4M1 application Flash only; BL616CL Flash is untouched.")
    print("Privacy: STA credentials, peer address, and payloads remain in process memory.")

    WORK_ROOT.mkdir(parents=True, exist_ok=True)
    work_dir = Path(tempfile.mkdtemp(prefix="unor4-tcp-resource-", dir=WORK_ROOT))
    bridge = None
    peer = None
    firewall_rule = None
    ra4_modified = False
    peer_output = ""
    primary_error: Exception | None = None
    event_log = work_dir / "windows_tcp_resource_peer.events"
    expected_exchanges = EXPECTED_PEER_EXCHANGES * (
        STRESS_CYCLES if args.stress else 1)
    try:
        image = compile_sketch(args)
        if args.skip_peer_firewall:
            print("TCP_RESOURCE_PEER:FIREWALL_SKIPPED")
        else:
            firewall_rule = configure_peer_firewall(peer_address, args.tcp_port)
        peer = launch_windows_peer(
            work_dir,
            peer_address,
            args.tcp_port,
            event_log,
            expected_exchanges,
        )
        peer_output = wait_for_peer_ready(peer, args.ready_timeout_ms / 1000.0)
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
        startup = capture_until(
            bridge,
            ("RA4_TCP_RESOURCE:READY",),
            args.ready_timeout_ms / 1000.0,
        )
        print_markers("RA4_TCP_RESOURCE_STARTUP", startup, "RA4_TCP_RESOURCE:")
        bridge.reset_input_buffer()
        start = (
            "START "
            + encode_start_value(ssid)
            + " "
            + encode_start_value(password)
            + " "
            + str(peer_address)
            + " "
            + str(args.tcp_port)
            + (" STRESS" if args.stress else "")
            + "\n"
        )
        bridge.write(start.encode("ascii"))
        bridge.flush()
        gate_timeout_seconds = max(
            args.gate_timeout_ms / 1000.0,
            240.0 if args.stress else 0.0,
        )
        runtime = capture_until(
            bridge,
            (
                "RA4_TCP_RESOURCE:INITIAL_CONNECT_PASS",
                "RA4_TCP_RESOURCE:INITIAL_ENDPOINT_PASS",
                "RA4_TCP_RESOURCE:INITIAL_ECHO_PASS",
                "RA4_TCP_RESOURCE:MIDDLE_CLOSE_PASS",
                "RA4_TCP_RESOURCE:MIDDLE_REUSE_PASS",
                "RA4_TCP_RESOURCE:REFUSED_CONNECT_PASS",
                "RA4_TCP_RESOURCE:REFUSED_RELEASE_PASS",
                "RA4_TCP_RESOURCE:CLOSE_ALL_PASS",
                "RA4_TCP_RESOURCE:RESULT=PASS",
            ),
            gate_timeout_seconds,
        )
        normalized_runtime = runtime.replace("\r", "")
        expected_cycles = STRESS_CYCLES if args.stress else 1
        repeated = (
            "RA4_TCP_RESOURCE:INITIAL_CONNECT_PASS",
            "RA4_TCP_RESOURCE:INITIAL_ECHO_PASS",
            "RA4_TCP_RESOURCE:MIDDLE_REUSE_PASS",
            "RA4_TCP_RESOURCE:REFUSED_RELEASE_PASS",
            "RA4_TCP_RESOURCE:CLOSE_ALL_PASS",
        )
        if any(normalized_runtime.count(marker) != expected_cycles
               for marker in repeated):
            raise RuntimeError("RA4M1 TCP resource gate missing repeated cycles")
        if args.stress and (
            any(f"RA4_TCP_RESOURCE:CYCLE_PASS={cycle}\n" not in normalized_runtime
                for cycle in range(1, STRESS_CYCLES + 1)) or
            "RA4_TCP_RESOURCE:ELAPSED_MS=" not in normalized_runtime
        ):
            raise RuntimeError("RA4M1 TCP resource gate missing reconnect markers")
        print_markers("RA4_TCP_RESOURCE_RUNTIME", runtime, "RA4_TCP_RESOURCE:")
        peer_output += wait_for_peer_events(
            event_log,
            ("TCP_RESOURCE_PEER:ECHO_PASS=" + str(expected_exchanges),),
            10.0,
        )
        print("RA4_WIFIS3_TCP_STRESS_GATE_PASS" if args.stress else
              "RA4_WIFIS3_TCP_RESOURCE_GATE_PASS")
    except MarkerCaptureError as error:
        print_markers("RA4_TCP_RESOURCE_DIAGNOSTICS", error.captured, "RA4_TCP_RESOURCE:")
        primary_error = error
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
            stop_windows_peer(peer, work_dir)
        remove_peer_firewall(firewall_rule)
        peer_output = read_peer_event_log(event_log) or peer_output
        print_markers("TCP_RESOURCE_PEER_RUNTIME", peer_output, "TCP_RESOURCE_PEER:")
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
        shutil.rmtree(work_dir, ignore_errors=True)

    if primary_error is not None:
        raise primary_error
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
