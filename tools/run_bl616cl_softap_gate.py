#!/usr/bin/env python3
"""Validate BL616CL SoftAP through the paired RA4M1 AT UART path.

The paired BL616CL bridge is the AP. The original ESP32-S3 UNO R4 is a
temporary STA peer. SSID/password material is generated in memory, omitted
from output, and never written to disk.
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
import inspect_bl616cl_gpio35 as telemetry
import run_esp32_sta_baseline_gate as esp32
import run_ra4_at_uart_gate as paired
from ra4_runtime_uart_route import open_runtime_route
from uno_r4_usb_identity import (
    resolve_baseline_esp32_port,
    resolve_paired_bl616cl_port,
    resolve_relay_port,
)


ROOT_DIR = Path(__file__).resolve().parents[1]
PAIRED_SCAN_RESTORE = ROOT_DIR / "tools" / "run_ra4_wifis3_scan_smoke.py"
AP_START_TELEMETRY = ROOT_DIR / "tools" / "inspect_bl616cl_gpio35.py"
AP_START_TELEMETRY_WINDOWS = (
    ROOT_DIR / "tools" / "inspect_bl616cl_ap_start_trace_windows.ps1"
)
WINDOWS_WSL_ROOT = r"\\wsl.localhost\Ubuntu-22.04"

TERMINAL = b"OK\r\n"
AT_REQUEST = b"AT\r\n"
STATUS_REQUEST = b"AT+GETSTATUS?\r\n"
STATUS_PATTERN = re.compile(rb"^\+GETSTATUS: ([0-9]{1,3})\r\nOK\r\n$")
MAC_PATTERN = re.compile(
    rb"^\+MACSOFTAP: [0-9a-f]{2}(?::[0-9a-f]{2}){5}\r\nOK\r\n$"
)


def quote_at_argument(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


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
    raise RuntimeError(f"AT response timed out with {len(received)} byte(s)")


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


def ensure_hidraw_access(
    prepare_via_wsl_root: bool,
    wsl_distribution: str,
) -> str:
    hidraw = telemetry.find_hidraw()
    if os.access(hidraw, os.R_OK | os.W_OK):
        return hidraw
    if not prepare_via_wsl_root:
        raise RuntimeError(
            f"HID node is not readable and writable: {hidraw}; rerun with "
            "--prepare-hidraw-via-wsl-root in this WSL USB/IP environment"
        )

    command = [
        "powershell.exe",
        "-NoProfile",
        "-Command",
        (
            f"wsl.exe -d {wsl_distribution} -u root -- "
            f"chmod a+rw {hidraw}"
        ),
    ]
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError(f"could not grant temporary access to {hidraw}")
    if not os.access(hidraw, os.R_OK | os.W_OK):
        raise RuntimeError(f"HID node is still not readable and writable: {hidraw}")
    return hidraw


def wait_for_watchdog_reset(
    timeout_seconds: float,
    wsl_distribution: str,
) -> str:
    """Require the paired USB device to disappear and then re-enumerate."""
    deadline = time.monotonic() + timeout_seconds
    observed_disconnect = False
    last_attach_attempt = 0.0
    while time.monotonic() < deadline:
        try:
            bridge_port = resolve_paired_bl616cl_port("auto")
            paired.require_port_identity(bridge_port, "2341", "1002", "00")
            if observed_disconnect:
                return bridge_port
        except RuntimeError:
            observed_disconnect = True
            now = time.monotonic()
            if now - last_attach_attempt >= 0.5:
                last_attach_attempt = now
                attach_paired_bridge_to_wsl(wsl_distribution)
        time.sleep(0.1)

    if observed_disconnect:
        raise RuntimeError(
            "paired BL616CL USB disappeared after AP start but did not "
            "re-enumerate before the watchdog recovery timeout"
        )
    raise RuntimeError(
        "paired BL616CL USB did not disappear before the watchdog recovery "
        "timeout; diagnostic watchdog reset was not observed"
    )


def attach_paired_bridge_to_wsl(wsl_distribution: str) -> None:
    """Best-effort USB/IP reattach after a BL616CL native USB reset."""
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
        f"usbipd wsl attach --busid $busid --distribution {wsl_distribution}"
    )
    try:
        subprocess.run(
            ["powershell.exe", "-NoProfile", "-Command", script],
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=5.0,
        )
    except subprocess.TimeoutExpired:
        # usbipd can finish the attach while its PowerShell client is still
        # waiting. The caller verifies the actual /dev identity in its loop.
        pass


def capture_ap_start_watchdog_telemetry(
    prepare_hidraw_via_wsl_root: bool,
    wsl_distribution: str,
) -> None:
    hidraw = ensure_hidraw_access(
        prepare_hidraw_via_wsl_root,
        wsl_distribution,
    )
    result = subprocess.run(
        [
            sys.executable,
            str(AP_START_TELEMETRY),
            "--hidraw",
            hidraw,
            "--ap-start-watchdog",
        ],
        check=False,
        capture_output=True,
        text=True,
        timeout=5.0,
    )
    if result.stdout:
        print(result.stdout, end="")
    if result.returncode != 0:
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
        raise RuntimeError("could not read AP-start watchdog telemetry")
    if (
        "enabled=1 state=3 " not in result.stdout and
        "enabled=1 state=4 " not in result.stdout
    ):
        raise RuntimeError(
            "AP-start watchdog telemetry did not report a watchdog reset"
        )


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
            restored_port = resolve_paired_bl616cl_port("auto")
            paired.require_port_identity(restored_port, "2341", "1002", "00")
            print(
                "AP_START_WINDOWS_TELEMETRY_CAPTURE_PASS "
                f"bridge_port={restored_port}"
            )
            return
        except RuntimeError:
            time.sleep(0.25)
    print("AP_START_WINDOWS_TELEMETRY_REATTACH_FAILED", file=sys.stderr)


def paired_transact(
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


def wait_for_paired_at_idle(
    bridge: serial.Serial,
    reference: serial.Serial,
    timeout_seconds: float,
) -> None:
    """Discard post-reset SDK output before the stateful AP setup begins."""
    deadline = time.monotonic() + timeout_seconds
    last_detail = "no response"
    while time.monotonic() < deadline:
        try:
            response = paired_transact(bridge, reference, AT_REQUEST, 0.5)
            last_detail = repr(response)
            if response == TERMINAL:
                return
        except RuntimeError as error:
            last_detail = str(error)
        time.sleep(0.1)
    raise RuntimeError(
        "BL616CL AT transport did not become idle before SoftAP setup "
        f"(last observation {last_detail})"
    )


def parse_status(response: bytes) -> int:
    match = STATUS_PATTERN.fullmatch(response)
    if match is None:
        raise RuntimeError("GETSTATUS did not match the expected AT framing")
    return int(match.group(1))


def poll_paired_status(
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
            paired_transact(bridge, reference, STATUS_REQUEST, response_timeout_seconds)
        )
        if not observed or observed[-1] != status:
            observed.append(status)
        if status == expected:
            return ((time.monotonic() - started_at) * 1000.0, observed)
        time.sleep(0.25)
    return (None, observed)


def upload_proxies(args: argparse.Namespace) -> None:
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
    esp32.compile_and_upload(
        argparse.Namespace(
            arduino_cli=args.arduino_cli,
            fqbn=esp32.DEFAULT_FQBN,
            port=args.esp32_port,
        ),
        esp32.PROXY_SKETCH,
        esp32.PROXY_BUILD_DIR,
        "ESP32_RA4_PROXY",
    )
    time.sleep(args.runtime_settle_ms / 1000.0)


def restore_scan_sketches(args: argparse.Namespace) -> None:
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
    restore_esp32_scan_sketch(args)


def restore_esp32_scan_sketch(args: argparse.Namespace) -> None:
    esp32_args = argparse.Namespace(
        arduino_cli=args.arduino_cli,
        fqbn=esp32.DEFAULT_FQBN,
        port=args.esp32_port,
        scan_startup_timeout_ms=100,
        scan_capture_timeout_ms=45000,
    )
    esp32.restore_scan_smoke(esp32_args)


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
    parser.add_argument("--response-timeout-ms", type=int, default=2000)
    parser.add_argument("--status-timeout-ms", type=int, default=20000)
    parser.add_argument("--runtime-settle-ms", type=int, default=2000)
    parser.add_argument(
        "--expect-ap-start-watchdog-reset",
        action="store_true",
        help=(
            "Treat an AP-start USB failure as an expected diagnostic event: "
            "require device disconnect/re-enumeration, read the persistent "
            "watchdog record, and verify post-reset AT recovery."
        ),
    )
    parser.add_argument("--watchdog-recovery-timeout-ms", type=int, default=20000)
    parser.add_argument(
        "--prepare-hidraw-via-wsl-root",
        action="store_true",
        help=(
            "Temporarily grant the current WSL hidraw node read/write access "
            "before reading the watchdog diagnostic feature page."
        ),
    )
    parser.add_argument(
        "--keep-proxy",
        action="store_true",
        help="Leave both RA4M1 boards running the temporary AT proxy.",
    )
    parser.add_argument(
        "--skip-proxy-upload",
        action="store_true",
        help=(
            "Reuse the paired and baseline RA4M1 AT proxy already running "
            "on the two boards instead of replacing either RA4M1 Sketch."
        ),
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
        or args.watchdog_recovery_timeout_ms <= 0
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
    print(f"ESP32-S3 peer:   {args.esp32_port} (2341:1002 original baseline)")
    print("Privacy: generated SoftAP credentials and raw AT responses stay in memory.")

    response_timeout = args.response_timeout_ms / 1000.0
    status_timeout = args.status_timeout_ms / 1000.0
    ssid = "uno-r4-" + secrets.token_hex(4)
    password = secrets.token_urlsafe(18)
    ap_started = False
    primary_error: Exception | None = None

    bridge: serial.Serial | None = None
    reference: serial.Serial | None = None
    peer: serial.Serial | None = None
    route = None
    ap_start_requested = False
    stage = "initial"
    watchdog_recovered = False
    try:
        if args.skip_proxy_upload:
            stage = "proxies_reused"
            print("BL616CL_SOFTAP_GATE_STAGE=PROXIES_REUSED")
        else:
            upload_proxies(args)
            stage = "proxies_uploaded"
            print("BL616CL_SOFTAP_GATE_STAGE=PROXIES_UPLOADED")
        args.bridge_port = resolve_paired_bl616cl_port("auto")
        args.esp32_port = resolve_baseline_esp32_port("auto")
        paired.require_port_identity(args.bridge_port, "2341", "1002", "00")
        esp32.require_port_identity(args.esp32_port)

        route = open_runtime_route(
            args.bridge_port,
            args.relay_port,
            args.legacy_reference_port,
            bridge_timeout=1.0,
            receiver_timeout=0.05,
        )
        bridge = route.bridge
        reference = route.receiver
        peer = esp32.open_serial(args.esp32_port, 0.1)
        bridge.reset_input_buffer()
        reference.reset_input_buffer()
        peer.reset_input_buffer()

        print("BL616CL_SOFTAP_GATE_STAGE=AT_IDLE")
        wait_for_paired_at_idle(bridge, reference, response_timeout)
        stage = "at_idle"
        print("BL616CL_SOFTAP_GATE_STAGE=WIFIMODE_AP")
        require_exact(
            paired_transact(
                bridge, reference, b"AT+WIFIMODE=2\r\n", response_timeout
            ),
            b"+WIFIMODE: \r\nOK\r\n",
            "BL616CL WIFIMODE=2",
        )
        stage = "wifimode_ap"
        print("BL616CL_SOFTAP_GATE_STAGE=SOFTAPCONFIG")
        require_exact(
            paired_transact(
                bridge,
                reference,
                b"AT+SOFTAPCONFIG=192.168.42.1,192.168.42.1,255.255.255.0\r\n",
                response_timeout,
            ),
            b"+SOFTAPCONFIG: \r\nOK\r\n",
            "BL616CL SOFTAPCONFIG",
        )
        stage = "softapconfig"

        begin_ap = (
            "AT+BEGINSOFTAP="
            f"{quote_at_argument(ssid)},{quote_at_argument(password)},1,0\r\n"
        ).encode("ascii")
        ap_start_requested = True
        stage = "beginsoftap_requested"
        print("BL616CL_SOFTAP_GATE_STAGE=BEGINSOFTAP")
        response = paired_transact(bridge, reference, begin_ap, response_timeout)
        require_exact(response, b"+BEGINSOFTAP: 1\r\nOK\r\n", "BL616CL BEGINSOFTAP")
        ap_started = True
        stage = "beginsoftap_accepted"
        print("BL616CL_SOFTAP_GATE_STAGE=AP_STATUS")

        started_ms, started_statuses = poll_paired_status(
            bridge, reference, 7, status_timeout, response_timeout
        )
        if started_ms is None:
            raise RuntimeError("BL616CL SoftAP did not reach GETSTATUS=7")
        require_exact(
            paired_transact(bridge, reference, b"AT+IPSOFTAP\r\n", response_timeout),
            b"+IPSOFTAP: 192.168.42.1\r\nOK\r\n",
            "BL616CL IPSOFTAP",
        )
        response = paired_transact(
            bridge, reference, b"AT+GETSOFTAPSSID?\r\n", response_timeout
        )
        expected_ssid = b"+GETSOFTAPSSID: " + ssid.encode("ascii") + b"\r\nOK\r\n"
        require_exact(response, expected_ssid, "BL616CL GETSOFTAPSSID")
        if not MAC_PATTERN.fullmatch(
            paired_transact(bridge, reference, b"AT+MACSOFTAP?\r\n", response_timeout)
        ):
            raise RuntimeError("BL616CL MACSOFTAP did not match the expected framing")

        require_exact(
            esp32.transact(peer, b"AT+WIFIMODE=1\r\n", response_timeout),
            b"+WIFIMODE: \r\nOK\r\n",
            "ESP32-S3 WIFIMODE=1",
        )
        esp32.validate_beginsta(
            esp32.transact(
                peer, esp32.build_beginsta_request(ssid, password), response_timeout
            )
        )
        sta_ms, sta_statuses = esp32.poll_status(
            peer, status_timeout, response_timeout, expected=3
        )
        if sta_ms is None:
            raise RuntimeError("ESP32-S3 STA did not reach GETSTATUS=3")

        client_ms, client_statuses = poll_paired_status(
            bridge, reference, 8, status_timeout, response_timeout
        )
        if client_ms is None:
            raise RuntimeError("BL616CL SoftAP did not reach GETSTATUS=8")

        require_exact(
            paired_transact(
                bridge, reference, b"AT+DISCONNECTSOFTAP\r\n", response_timeout
            ),
            TERMINAL,
            "BL616CL DISCONNECTSOFTAP",
        )
        ap_started = False
        stopped_ms, stopped_statuses = poll_paired_status(
            bridge, reference, 0, status_timeout, response_timeout
        )
        if stopped_ms is None:
            raise RuntimeError("BL616CL SoftAP did not return to GETSTATUS=0")
        peer_stop_ms, peer_stop_statuses = esp32.poll_status(
            peer, status_timeout, response_timeout, expected=0
        )
        if peer_stop_ms is None:
            raise RuntimeError("ESP32-S3 STA did not return to GETSTATUS=0")

        print(f"BL616CL_SOFTAP_START_STATUSES={','.join(map(str, started_statuses))}")
        print(f"BL616CL_SOFTAP_STARTED_MS={started_ms:.3f}")
        print(f"ESP32_STA_CONNECT_STATUSES={','.join(map(str, sta_statuses))}")
        print(f"ESP32_STA_CONNECTED_MS={sta_ms:.3f}")
        print(f"BL616CL_SOFTAP_CLIENT_STATUSES={','.join(map(str, client_statuses))}")
        print(f"BL616CL_SOFTAP_CLIENT_MS={client_ms:.3f}")
        print(f"BL616CL_SOFTAP_STOP_STATUSES={','.join(map(str, stopped_statuses))}")
        print(f"BL616CL_SOFTAP_STOPPED_MS={stopped_ms:.3f}")
        print(f"ESP32_STA_STOP_STATUSES={','.join(map(str, peer_stop_statuses))}")
        print(f"ESP32_STA_STOPPED_MS={peer_stop_ms:.3f}")
        print("BL616CL_SOFTAP_GATE_PASS")
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as error:
        primary_error = error
        print(
            "BL616CL_SOFTAP_GATE_FAILURE="
            f"stage={stage} ap_start_requested={int(ap_start_requested)}",
            file=sys.stderr,
        )
        if args.expect_ap_start_watchdog_reset and ap_start_requested:
            try:
                if route is not None:
                    route.close()
                    route = None
                    bridge = None
                    reference = None
                args.bridge_port = wait_for_watchdog_reset(
                    args.watchdog_recovery_timeout_ms / 1000.0,
                    args.wsl_distribution,
                )
                paired.require_port_identity(args.bridge_port, "2341", "1002", "00")
                print(
                    "BL616CL_AP_START_WATCHDOG_USB_RECOVERED "
                    f"bridge_port={args.bridge_port}"
                )
                capture_ap_start_watchdog_telemetry(
                    args.prepare_hidraw_via_wsl_root,
                    args.wsl_distribution,
                )
                route = open_runtime_route(
                    args.bridge_port,
                    args.relay_port,
                    args.legacy_reference_port,
                    bridge_timeout=1.0,
                    receiver_timeout=0.05,
                )
                bridge = route.bridge
                reference = route.receiver
                wait_for_paired_at_idle(bridge, reference, response_timeout)
                ap_started = False
                watchdog_recovered = True
                primary_error = None
                print("BL616CL_AP_START_WATCHDOG_RECOVERY_PASS")
            except (
                OSError,
                RuntimeError,
                serial.SerialException,
                subprocess.SubprocessError,
            ) as watchdog_error:
                primary_error = watchdog_error
                print("BL616CL_AP_START_WATCHDOG_RECOVERY_FAILED", file=sys.stderr)
        if (
            primary_error is not None
            and args.capture_ap_start_telemetry
            and ap_start_requested
        ):
            try:
                capture_ap_start_telemetry()
            except (OSError, subprocess.SubprocessError):
                print("AP_START_TELEMETRY_CAPTURE_FAILED", file=sys.stderr)
        if (
            primary_error is not None
            and args.capture_ap_start_telemetry_windows
            and ap_start_requested
        ):
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
                    paired_transact(
                        bridge, reference, b"AT+DISCONNECTSOFTAP\r\n", response_timeout
                    )
                    print("BL616CL_SOFTAP_CLEANUP_STOP_SENT")
                except (OSError, RuntimeError, serial.SerialException):
                    print("BL616CL_SOFTAP_CLEANUP_STOP_FAILED", file=sys.stderr)
        if peer is not None and peer.is_open:
            peer.close()
        if route is not None:
            route.close()

        if not args.keep_proxy:
            try:
                restore_scan_sketches(args)
                print("RA4_SCAN_RESTORE_PASS")
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
        raise primary_error
    if args.expect_ap_start_watchdog_reset and not watchdog_recovered:
        raise RuntimeError(
            "AP-start watchdog diagnostic expected a reset recovery but "
            "the SoftAP gate completed without one"
        )
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
