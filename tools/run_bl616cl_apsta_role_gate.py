#!/usr/bin/env python3
"""Validate BL616CL AP+STA roles against a controlled ESP32-S3 upstream AP.

The original ESP32-S3 board serves only as a temporary upstream SoftAP. The
paired BL616CL bridge joins that AP in WIFIMODE=3, then starts its own SoftAP.
STA evidence is checked before and after the local AP starts without printing
credentials, raw responses, SSIDs, or BSSIDs.
"""

from __future__ import annotations

import argparse
import os
import re
import secrets
import subprocess
import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_bl616cl_apsta_gate as apsta
import run_ra4_at_uart_gate as paired
import run_esp32_sta_baseline_gate as esp32
from ra4_runtime_uart_route import open_runtime_route
from uno_r4_usb_identity import (
    resolve_baseline_esp32_port,
    resolve_paired_bl616cl_port,
    resolve_relay_port,
)


ROOT_DIR = Path(__file__).resolve().parents[1]
PAIRED_SCAN_RESTORE = ROOT_DIR / "tools" / "run_ra4_wifis3_scan_smoke.py"
ESP32_CONTROLLER = ROOT_DIR / "tools" / "esp32_softap_controller.py"
AP_START_TELEMETRY = ROOT_DIR / "tools" / "inspect_bl616cl_gpio35.py"
AP_START_TELEMETRY_WINDOWS = (
    ROOT_DIR / "tools" / "inspect_bl616cl_ap_start_windows.ps1"
)
WINDOWS_WSL_ROOT = r"\\wsl.localhost\Ubuntu-22.04"
DEFAULT_CREDENTIAL_ENV = (
    Path.home() / ".config" / "uno-r4-wifi-bridge" / "esp32-softap-test.env"
)
DEFAULT_CONTROL_SOCKET = Path("/tmp/uno-r4-esp32-softap.sock")

TERMINAL = b"OK\r\n"
STATUS_REQUEST = b"AT+GETSTATUS?\r\n"
STATUS_PATTERN = re.compile(rb"^\+GETSTATUS: ([0-9]{1,3})\r\nOK\r\n$")
MAXIMUM_IDEMPOTENT_ATTEMPTS = 3
IDEMPOTENT_RETRY_DELAY_SECONDS = 0.25


class AtResponseTimeout(RuntimeError):
    def __init__(self, response: bytes) -> None:
        super().__init__(f"AT response timed out with {len(response)} byte(s)")
        self.response = response


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def read_until_terminal(
    link: serial.Serial, timeout_seconds: float, maximum_bytes: int = 512
) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        remaining = maximum_bytes - len(received)
        if remaining <= 0:
            raise RuntimeError("AT response exceeded its byte limit")
        payload = link.read(min(remaining, 256))
        if payload:
            received.extend(payload)
            if received.endswith(TERMINAL):
                return bytes(received)
    raise AtResponseTimeout(bytes(received))


def print_begin_sta_response_diagnostic(response: bytes) -> None:
    """Report BEGINSTA framing without exposing credentials or raw payload."""
    marker = re.search(rb"\+BEGINSTA:\s*(-?[0-9]+)", response)
    crlf_count = response.count(b"\r\n")
    prefix_size = marker.start() if marker else len(response)
    suffix_size = len(response) - marker.end() if marker else 0
    print(
        "BL616CL_BEGINSTA_RESPONSE_DIAGNOSTIC="
        f"bytes={len(response)} "
        f"has_beginsta_marker={int(marker is not None)} "
        f"result={(marker.group(1).decode('ascii') if marker else 'none')} "
        f"prefix_bytes={prefix_size} "
        f"suffix_bytes={suffix_size} "
        f"ends_ok={int(response.endswith(TERMINAL))} "
        f"crlf_count={crlf_count}"
    )


def transact(
    bridge: serial.Serial,
    reference: serial.Serial,
    request: bytes,
    timeout_seconds: float,
) -> bytes:
    reference.reset_input_buffer()
    bridge.write(request)
    bridge.flush()
    return read_until_terminal(reference, timeout_seconds)


def require_exact(response: bytes, expected: bytes, label: str) -> None:
    if response != expected:
        raise RuntimeError(f"{label} did not match the expected AT framing")


def transact_exact_idempotent(
    bridge: serial.Serial,
    reference: serial.Serial,
    request: bytes,
    expected: bytes,
    timeout_seconds: float,
    label: str,
) -> None:
    """Retry only settings that can be safely applied more than once."""
    response = b""
    for attempt in range(MAXIMUM_IDEMPOTENT_ATTEMPTS):
        try:
            response = transact(bridge, reference, request, timeout_seconds)
        except RuntimeError:
            if attempt + 1 == MAXIMUM_IDEMPOTENT_ATTEMPTS:
                raise
        else:
            if response == expected:
                return
            if attempt + 1 == MAXIMUM_IDEMPOTENT_ATTEMPTS:
                require_exact(response, expected, label)
        time.sleep(IDEMPOTENT_RETRY_DELAY_SECONDS)


def parse_status(response: bytes) -> int:
    match = STATUS_PATTERN.fullmatch(response)
    if match is None:
        raise RuntimeError("GETSTATUS did not match the expected AT framing")
    return int(match.group(1))


def poll_status(
    bridge: serial.Serial,
    reference: serial.Serial,
    expected: int,
    timeout_seconds: float,
    response_timeout_seconds: float,
) -> tuple[float | None, list[int]]:
    started_at = time.monotonic()
    deadline = started_at + timeout_seconds
    observed: list[int] = []
    while time.monotonic() < deadline:
        status = parse_status(
            transact(bridge, reference, STATUS_REQUEST, response_timeout_seconds)
        )
        if not observed or observed[-1] != status:
            observed.append(status)
        if status == expected:
            return ((time.monotonic() - started_at) * 1000.0, observed)
        time.sleep(0.25)
    return (None, observed)


def start_esp32_controller(args: argparse.Namespace) -> subprocess.Popen[str]:
    if args.credential_env.exists() or args.control_socket.exists():
        raise RuntimeError("controlled ESP32 SoftAP state from a prior run still exists")

    controller = subprocess.Popen(
        [
            sys.executable,
            "-u",
            str(ESP32_CONTROLLER),
            "--port",
            args.esp32_port,
            "--arduino-cli",
            args.arduino_cli,
            "--credential-env",
            str(args.credential_env),
            "--control-socket",
            str(args.control_socket),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    assert controller.stdout is not None
    deadline = time.monotonic() + args.controller_ready_timeout_ms / 1000.0
    while time.monotonic() < deadline:
        line = controller.stdout.readline()
        if line == "":
            if controller.poll() is not None:
                raise RuntimeError("controlled ESP32 SoftAP controller exited before ready")
            continue
        if line.rstrip("\r\n") == "ESP32_SOFTAP_CONTROLLER_READY":
            print("ESP32_SOFTAP_CONTROLLER_READY")
            return controller
    controller.terminate()
    controller.wait(timeout=15.0)
    raise RuntimeError("controlled ESP32 SoftAP controller did not become ready")


def stop_esp32_controller(controller: subprocess.Popen[str] | None) -> None:
    if controller is None:
        return
    if controller.poll() is None:
        controller.terminate()
    try:
        controller.wait(timeout=60.0)
    except subprocess.TimeoutExpired:
        controller.kill()
        controller.wait(timeout=15.0)
        raise RuntimeError("controlled ESP32 SoftAP controller did not stop cleanly")
    if controller.returncode != 0:
        raise RuntimeError("controlled ESP32 SoftAP controller cleanup failed")
    print("ESP32_SOFTAP_CONTROLLER_STOPPED")


def upload_paired_proxy(args: argparse.Namespace) -> None:
    run_command(
        [
            sys.executable,
            str(ROOT_DIR / "tools" / "run_ra4_at_uart_gate.py"),
            "--bridge-port",
            args.bridge_port,
            "--relay-port",
            args.relay_port,
            "--arduino-cli",
            args.arduino_cli,
            "--skip-status-check",
        ]
    )
    time.sleep(args.runtime_settle_ms / 1000.0)


def restore_paired_scan(args: argparse.Namespace) -> None:
    run_command(
        [
            sys.executable,
            str(PAIRED_SCAN_RESTORE),
            "--bridge-port",
            args.bridge_port,
            "--relay-port",
            args.relay_port,
            "--arduino-cli",
            args.arduino_cli,
        ]
    )
    print("RA4_SCAN_RESTORE_PASS")


def capture_ap_start_telemetry() -> None:
    """Best-effort HID snapshot after the AP-start AT call stops responding."""
    result = subprocess.run(
        [sys.executable, str(AP_START_TELEMETRY), "--ap-start"],
        check=False,
        capture_output=True,
        text=True,
        timeout=5.0,
    )
    if result.stdout:
        print(result.stdout, end="")
    if result.returncode != 0:
        print("AP_START_TELEMETRY_CAPTURE_FAILED", file=sys.stderr)
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)


def capture_ap_start_telemetry_windows(
    bridge_port: str,
    wsl_distribution: str,
) -> None:
    """Temporarily return the paired USB device to Windows for HID access."""
    windows_script = WINDOWS_WSL_ROOT + str(AP_START_TELEMETRY_WINDOWS)
    script = (
        "$ErrorActionPreference = 'Stop'; "
        "$devices = @((usbipd state | ConvertFrom-Json).Devices | "
        "Where-Object { $_.BusId -ne $null -and "
        "$_.InstanceId.EndsWith('\\BL616CL-PROVISIONAL', "
        "[System.StringComparison]::OrdinalIgnoreCase) }); "
        "if ($devices.Count -ne 1) { "
        "throw \"expected exactly one paired BL616CL USB device, found $($devices.Count)\" "
        "}; "
        "$busid = $devices[0].BusId; "
        "usbipd wsl detach --busid $busid; "
        "try { "
        "Start-Sleep -Milliseconds 750; "
        f"& '{windows_script}'; "
        "if ($LASTEXITCODE -ne 0) { "
        "throw \"Windows AP-start telemetry helper failed with exit code $LASTEXITCODE\" "
        "} "
        "} finally { "
        "usbipd wsl attach --busid $busid "
        f"--distribution {wsl_distribution} "
        "}"
    )
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=20.0,
    )
    if result.stdout:
        print(result.stdout, end="")
    if result.returncode != 0:
        print("AP_START_WINDOWS_TELEMETRY_CAPTURE_FAILED", file=sys.stderr)
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
        return

    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        try:
            paired.require_port_identity(bridge_port, "2341", "1002", "00")
            print("AP_START_WINDOWS_TELEMETRY_CAPTURE_PASS")
            return
        except RuntimeError:
            time.sleep(0.25)
    print("AP_START_WINDOWS_TELEMETRY_REATTACH_FAILED", file=sys.stderr)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--relay-port", default="auto")
    parser.add_argument(
        "--reference-port",
        dest="relay_port",
        default=argparse.SUPPRESS,
        help=argparse.SUPPRESS,
    )
    parser.add_argument("--legacy-reference-port")
    parser.add_argument("--esp32-port", default="auto")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--credential-env", type=Path, default=DEFAULT_CREDENTIAL_ENV)
    parser.add_argument("--control-socket", type=Path, default=DEFAULT_CONTROL_SOCKET)
    parser.add_argument("--response-timeout-ms", type=int, default=2000)
    parser.add_argument("--status-timeout-ms", type=int, default=30000)
    parser.add_argument("--runtime-settle-ms", type=int, default=2000)
    parser.add_argument("--controller-ready-timeout-ms", type=int, default=90000)
    parser.add_argument(
        "--keep-proxy",
        action="store_true",
        help="Leave the paired RA4M1 running the temporary AT proxy.",
    )
    parser.add_argument(
        "--capture-ap-start-telemetry",
        action="store_true",
        help=(
            "Read the BL616CL HID AP-start diagnostic page when the "
            "BEGINSOFTAP transaction fails."
        ),
    )
    parser.add_argument(
        "--capture-ap-start-telemetry-windows",
        action="store_true",
        help=(
            "Temporarily return the paired USB device to Windows and read "
            "the AP-start HID diagnostic page after BEGINSOFTAP fails."
        ),
    )
    parser.add_argument("--wsl-distribution", default="Ubuntu-22.04")
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    args.relay_port = resolve_relay_port(args.relay_port)
    args.esp32_port = resolve_baseline_esp32_port(args.esp32_port)
    if (
        args.response_timeout_ms <= 0
        or args.status_timeout_ms <= 0
        or args.runtime_settle_ms < 0
        or args.controller_ready_timeout_ms <= 0
    ):
        raise RuntimeError("invalid timeout")
    if args.capture_ap_start_telemetry and args.capture_ap_start_telemetry_windows:
        raise RuntimeError("choose only one AP-start telemetry transport")

    paired.require_port_identity(args.bridge_port, "2341", "1002", "00")
    paired.require_port_identity(args.relay_port, "0403", "6010", "00")
    if args.legacy_reference_port is not None:
        paired.require_port_identity(args.legacy_reference_port, "0403", "6010", "00")
    esp32.require_port_identity(args.esp32_port)
    print(f"BL616CL bridge: {args.bridge_port} (2341:1002)")
    print(f"Relay:           {args.relay_port} (0403:6010 interface 00)")
    print(f"ESP32-S3 AP:     {args.esp32_port} (2341:1002 original baseline)")
    print("Privacy: generated AP credentials and all STA identity responses stay in memory.")

    response_timeout = args.response_timeout_ms / 1000.0
    status_timeout = args.status_timeout_ms / 1000.0
    local_ap_ssid = "uno-r4-" + secrets.token_hex(4)
    local_ap_password = secrets.token_urlsafe(18)
    controller: subprocess.Popen[str] | None = None
    bridge: serial.Serial | None = None
    reference: serial.Serial | None = None
    route = None
    proxy_uploaded = False
    ap_started = False
    sta_requested = False
    mode_changed = False
    primary_error: Exception | None = None
    sta_statuses: list[int] = []
    ap_statuses: list[int] = []
    ap_start_requested = False

    try:
        controller = start_esp32_controller(args)
        upstream_ssid, upstream_password = apsta.load_private_credentials(
            args.credential_env
        )
        upload_paired_proxy(args)
        proxy_uploaded = True

        route = open_runtime_route(
            args.bridge_port,
            args.relay_port,
            args.legacy_reference_port,
            bridge_timeout=1.0,
            receiver_timeout=0.05,
        )
        bridge = route.bridge
        reference = route.receiver
        bridge.reset_input_buffer()
        reference.reset_input_buffer()

        print("BL616CL_APSTA_ROLE_STEP=MODE_APSTA")
        transact_exact_idempotent(
            bridge,
            reference,
            b"AT+WIFIMODE=3\r\n",
            b"+WIFIMODE: \r\nOK\r\n",
            response_timeout,
            "BL616CL WIFIMODE=3",
        )
        mode_changed = True
        print("BL616CL_APSTA_ROLE_STEP=RECONNECT_DISABLE")
        transact_exact_idempotent(
            bridge,
            reference,
            b"AT+RECONNECT=0\r\n",
            TERMINAL,
            response_timeout,
            "BL616CL RECONNECT=0",
        )
        begin_sta = (
            "AT+BEGINSTA="
            + apsta.quote_at_argument(upstream_ssid)
            + ("," + apsta.quote_at_argument(upstream_password) if upstream_password else "")
            + "\r\n"
        ).encode("ascii")
        print("BL616CL_APSTA_ROLE_STEP=BEGIN_STA")
        response = transact(bridge, reference, begin_sta, response_timeout)
        print_begin_sta_response_diagnostic(response)
        if not re.fullmatch(rb"^\+BEGINSTA: [0-9]{1,3}\r\nOK\r\n$", response):
            raise RuntimeError("BL616CL BEGINSTA did not match the expected AT framing")
        sta_requested = True
        print("BL616CL_APSTA_ROLE_STEP=WAIT_STA")
        sta_ms, sta_statuses = poll_status(
            bridge, reference, 3, status_timeout, response_timeout
        )
        if sta_ms is None:
            raise RuntimeError("BL616CL STA did not reach GETSTATUS=3")
        print("BL616CL_APSTA_ROLE_STEP=CHECK_STA_BEFORE_AP")
        apsta.confirm_sta(bridge, reference, upstream_ssid, response_timeout)

        print("BL616CL_APSTA_ROLE_STEP=SOFTAP_CONFIG")
        transact_exact_idempotent(
            bridge,
            reference,
            b"AT+SOFTAPCONFIG=192.168.42.1,192.168.42.1,255.255.255.0\r\n",
            b"+SOFTAPCONFIG: \r\nOK\r\n",
            response_timeout,
            "BL616CL SOFTAPCONFIG",
        )
        begin_ap = (
            "AT+BEGINSOFTAP="
            f"{apsta.quote_at_argument(local_ap_ssid)},"
            f"{apsta.quote_at_argument(local_ap_password)},1,0\r\n"
        ).encode("ascii")
        print("BL616CL_APSTA_ROLE_STEP=BEGIN_SOFTAP")
        ap_start_requested = True
        try:
            ap_response = transact(bridge, reference, begin_ap, response_timeout)
        except AtResponseTimeout:
            raise
        require_exact(
            ap_response,
            b"+BEGINSOFTAP: 1\r\nOK\r\n",
            "BL616CL BEGINSOFTAP",
        )
        ap_started = True
        print("BL616CL_APSTA_ROLE_STEP=WAIT_SOFTAP")
        ap_ms, ap_statuses = poll_status(
            bridge, reference, 7, status_timeout, response_timeout
        )
        if ap_ms is None:
            raise RuntimeError("BL616CL AP+STA did not reach GETSTATUS=7")

        print("BL616CL_APSTA_ROLE_STEP=CHECK_STA_AFTER_AP")
        apsta.confirm_sta(bridge, reference, upstream_ssid, response_timeout)

        print("BL616CL_APSTA_ROLE_STEP=STOP_SOFTAP")
        require_exact(
            transact(bridge, reference, b"AT+DISCONNECTSOFTAP\r\n", response_timeout),
            TERMINAL,
            "BL616CL DISCONNECTSOFTAP",
        )
        ap_started = False
        print("BL616CL_APSTA_ROLE_STEP=DISCONNECT_STA")
        require_exact(
            transact(bridge, reference, b"AT+DISCONNECT\r\n", response_timeout),
            b"+DISCONNECT: \r\nOK\r\n",
            "BL616CL DISCONNECT",
        )
        sta_requested = False

        print(f"BL616CL_APSTA_ROLE_STA_STATUSES={','.join(map(str, sta_statuses))}")
        print(f"BL616CL_APSTA_ROLE_STA_CONNECTED_MS={sta_ms:.3f}")
        print(f"BL616CL_APSTA_ROLE_AP_STATUSES={','.join(map(str, ap_statuses))}")
        print(f"BL616CL_APSTA_ROLE_AP_STARTED_MS={ap_ms:.3f}")
        print("BL616CL_APSTA_ROLE_GATE_PASS")
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as error:
        primary_error = error
        if args.capture_ap_start_telemetry and ap_start_requested:
            try:
                capture_ap_start_telemetry()
            except (OSError, subprocess.SubprocessError):
                print("AP_START_TELEMETRY_CAPTURE_FAILED", file=sys.stderr)
        if args.capture_ap_start_telemetry_windows and ap_start_requested:
            try:
                if route is not None:
                    route.close()
                    route = None
                    bridge = None
                    reference = None
                capture_ap_start_telemetry_windows(
                    args.bridge_port,
                    args.wsl_distribution,
                )
            except (OSError, subprocess.SubprocessError):
                print("AP_START_WINDOWS_TELEMETRY_CAPTURE_FAILED", file=sys.stderr)
    finally:
        if bridge is not None and reference is not None and bridge.is_open and reference.is_open:
            if ap_started:
                try:
                    transact(
                        bridge, reference, b"AT+DISCONNECTSOFTAP\r\n", response_timeout
                    )
                    print("BL616CL_APSTA_ROLE_CLEANUP_AP_STOP_SENT")
                except (OSError, RuntimeError, serial.SerialException):
                    print("BL616CL_APSTA_ROLE_CLEANUP_AP_STOP_FAILED", file=sys.stderr)
            if sta_requested:
                try:
                    transact(bridge, reference, b"AT+DISCONNECT\r\n", response_timeout)
                    print("BL616CL_APSTA_ROLE_CLEANUP_STA_DISCONNECT_SENT")
                except (OSError, RuntimeError, serial.SerialException):
                    print("BL616CL_APSTA_ROLE_CLEANUP_STA_DISCONNECT_FAILED", file=sys.stderr)
            if mode_changed:
                try:
                    require_exact(
                        transact(bridge, reference, b"AT+WIFIMODE=1\r\n", response_timeout),
                        b"+WIFIMODE: \r\nOK\r\n",
                        "cleanup WIFIMODE=1",
                    )
                    print("BL616CL_APSTA_ROLE_CLEANUP_MODE_STA_RESTORED")
                except (OSError, RuntimeError, serial.SerialException):
                    print(
                        "BL616CL_APSTA_ROLE_CLEANUP_MODE_STA_RESTORE_FAILED",
                        file=sys.stderr,
                    )
        if route is not None:
            route.close()

        try:
            stop_esp32_controller(controller)
        except RuntimeError as error:
            if primary_error is None:
                primary_error = error
            else:
                print("ESP32_SOFTAP_CONTROLLER_CLEANUP_FAILED", file=sys.stderr)
        if proxy_uploaded and not args.keep_proxy:
            try:
                restore_paired_scan(args)
            except (
                OSError,
                RuntimeError,
                serial.SerialException,
                subprocess.SubprocessError,
            ) as error:
                if primary_error is None:
                    primary_error = error
                else:
                    print("RA4_SCAN_RESTORE_FAILED", file=sys.stderr)

    if primary_error is not None:
        if sta_statuses:
            print(
                "BL616CL_APSTA_ROLE_STA_FAILURE_STATUSES="
                + ",".join(map(str, sta_statuses))
            )
        if ap_statuses:
            print(
                "BL616CL_APSTA_ROLE_AP_FAILURE_STATUSES="
                + ",".join(map(str, ap_statuses))
            )
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
