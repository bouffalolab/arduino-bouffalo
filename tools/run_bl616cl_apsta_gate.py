#!/usr/bin/env python3
"""Validate simultaneous BL616CL STA association and SoftAP service.

Private upstream STA credentials are read only from the local mode-600 file.
They never appear in command output, error text, source-controlled files, or
the generated test SoftAP credentials.
"""

from __future__ import annotations

import argparse
import os
import re
import secrets
import stat
import subprocess
import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
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
DEFAULT_CREDENTIAL_ENV = (
    Path.home() / ".config" / "uno-r4-wifi-bridge" / "sta-test.env"
)

TERMINAL = b"OK\r\n"
STATUS_REQUEST = b"AT+GETSTATUS?\r\n"
STATUS_PATTERN = re.compile(rb"^\+GETSTATUS: ([0-9]{1,3})\r\nOK\r\n$")
RSSI_PATTERN = re.compile(rb"^\+GETRSSI: (-?[0-9]{1,3})\r\nOK\r\n$")
BSSID_PATTERN = re.compile(
    rb"^\+GETBSSID: [0-9a-f]{2}(?::[0-9a-f]{2}){5}\r\nOK\r\n$"
)


def quote_at_argument(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def require_credential(value: str | None, maximum_bytes: int) -> str:
    if not value:
        raise RuntimeError("private STA credential is absent or empty")
    try:
        encoded = value.encode("ascii", "strict")
    except UnicodeEncodeError as error:
        raise RuntimeError("private STA credential is not ASCII") from error
    if len(encoded) > maximum_bytes or any(
        byte in (0x00, 0x0A, 0x0D) for byte in encoded
    ):
        raise RuntimeError("private STA credential has an invalid length or control byte")
    return value


def load_private_credentials(path: Path) -> tuple[str, str | None]:
    metadata = path.stat()
    if (
        metadata.st_uid != os.getuid()
        or stat.S_IMODE(metadata.st_mode) != 0o600
        or not stat.S_ISREG(metadata.st_mode)
    ):
        raise RuntimeError("private STA credential file does not meet ownership/mode policy")

    values: dict[str, str] = {}
    for line in path.read_text(encoding="ascii").splitlines():
        key, separator, value = line.partition("=")
        if separator != "=" or key not in {"BL616CL_STA_SSID", "BL616CL_STA_PASSWORD"}:
            raise RuntimeError("private STA credential file has an unsupported format")
        if key in values:
            raise RuntimeError("private STA credential file has duplicate keys")
        values[key] = value
    ssid = require_credential(values.get("BL616CL_STA_SSID"), 32)
    password_value = values.get("BL616CL_STA_PASSWORD")
    password = (
        require_credential(password_value, 64)
        if password_value is not None
        else None
    )
    return ssid, password


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


def confirm_sta(
    bridge: serial.Serial,
    reference: serial.Serial,
    expected_ssid: str,
    response_timeout_seconds: float,
) -> None:
    expected = b"+GETSSID: " + expected_ssid.encode("ascii") + b"\r\nOK\r\n"
    require_exact(
        paired_transact(bridge, reference, b"AT+GETSSID?\r\n", response_timeout_seconds),
        expected,
        "BL616CL GETSSID",
    )
    rssi_response = paired_transact(
        bridge, reference, b"AT+GETRSSI?\r\n", response_timeout_seconds
    )
    rssi_match = RSSI_PATTERN.fullmatch(rssi_response)
    if rssi_match is None or not -127 <= int(rssi_match.group(1)) <= 0:
        raise RuntimeError("BL616CL GETRSSI did not provide an associated-STA RSSI")
    if not BSSID_PATTERN.fullmatch(
        paired_transact(bridge, reference, b"AT+GETBSSID?\r\n", response_timeout_seconds)
    ):
        raise RuntimeError("BL616CL GETBSSID did not provide an associated-STA BSSID")


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
    esp32.restore_scan_smoke(
        argparse.Namespace(
            arduino_cli=args.arduino_cli,
            fqbn=esp32.DEFAULT_FQBN,
            port=args.esp32_port,
            scan_startup_timeout_ms=100,
            scan_capture_timeout_ms=45000,
        )
    )


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
    parser.add_argument("--credential-env", type=Path, default=DEFAULT_CREDENTIAL_ENV)
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--response-timeout-ms", type=int, default=2000)
    parser.add_argument("--status-timeout-ms", type=int, default=30000)
    parser.add_argument("--runtime-settle-ms", type=int, default=2000)
    parser.add_argument(
        "--keep-proxy",
        action="store_true",
        help="Leave both RA4M1 boards running the temporary AT proxy.",
    )
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
    ):
        raise RuntimeError("invalid timeout")

    sta_ssid, sta_password = load_private_credentials(args.credential_env)
    paired.require_port_identity(args.bridge_port, "2341", "1002", "00")
    paired.require_port_identity(args.relay_port, "0403", "6010", "00")
    if args.legacy_reference_port is not None:
        paired.require_port_identity(args.legacy_reference_port, "0403", "6010", "00")
    esp32.require_port_identity(args.esp32_port)
    print(f"BL616CL bridge: {args.bridge_port} (2341:1002)")
    print(f"Relay:           {args.relay_port} (0403:6010 interface 00)")
    print(f"ESP32-S3 peer:   {args.esp32_port} (2341:1002 original baseline)")
    print("Privacy: private STA and generated SoftAP credentials stay in memory.")

    response_timeout = args.response_timeout_ms / 1000.0
    status_timeout = args.status_timeout_ms / 1000.0
    ap_ssid = "uno-r4-" + secrets.token_hex(4)
    ap_password = secrets.token_urlsafe(18)
    ap_started = False
    sta_requested = False
    wifi_mode_changed = False
    primary_error: Exception | None = None
    sta_statuses: list[int] = []
    ap_statuses: list[int] = []
    peer_statuses: list[int] = []
    client_statuses: list[int] = []

    bridge: serial.Serial | None = None
    reference: serial.Serial | None = None
    peer: serial.Serial | None = None
    route = None
    try:
        upload_proxies(args)
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

        require_exact(
            paired_transact(bridge, reference, b"AT+WIFIMODE=3\r\n", response_timeout),
            b"+WIFIMODE: \r\nOK\r\n",
            "BL616CL WIFIMODE=3",
        )
        wifi_mode_changed = True
        require_exact(
            paired_transact(bridge, reference, b"AT+RECONNECT=0\r\n", response_timeout),
            TERMINAL,
            "BL616CL RECONNECT=0",
        )
        begin_sta = (
            "AT+BEGINSTA="
            + quote_at_argument(sta_ssid)
            + ("," + quote_at_argument(sta_password) if sta_password else "")
            + "\r\n"
        ).encode("ascii")
        response = paired_transact(bridge, reference, begin_sta, response_timeout)
        if not re.fullmatch(rb"^\+BEGINSTA: [0-9]{1,3}\r\nOK\r\n$", response):
            raise RuntimeError("BL616CL BEGINSTA did not match the expected AT framing")
        sta_requested = True
        sta_ms, sta_statuses = poll_paired_status(
            bridge, reference, 3, status_timeout, response_timeout
        )
        if sta_ms is None:
            raise RuntimeError("BL616CL STA did not reach GETSTATUS=3")
        confirm_sta(bridge, reference, sta_ssid, response_timeout)

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
        begin_ap = (
            "AT+BEGINSOFTAP="
            f"{quote_at_argument(ap_ssid)},{quote_at_argument(ap_password)},1,0\r\n"
        ).encode("ascii")
        require_exact(
            paired_transact(bridge, reference, begin_ap, response_timeout),
            b"+BEGINSOFTAP: 1\r\nOK\r\n",
            "BL616CL BEGINSOFTAP",
        )
        ap_started = True
        ap_ms, ap_statuses = poll_paired_status(
            bridge, reference, 7, status_timeout, response_timeout
        )
        if ap_ms is None:
            raise RuntimeError("BL616CL AP+STA did not reach GETSTATUS=7")

        require_exact(
            esp32.transact(peer, b"AT+WIFIMODE=1\r\n", response_timeout),
            b"+WIFIMODE: \r\nOK\r\n",
            "ESP32-S3 WIFIMODE=1",
        )
        esp32.validate_beginsta(
            esp32.transact(
                peer,
                esp32.build_beginsta_request(ap_ssid, ap_password),
                response_timeout,
            )
        )
        peer_ms, peer_statuses = esp32.poll_status(
            peer, status_timeout, response_timeout, expected=3
        )
        if peer_ms is None:
            raise RuntimeError("ESP32-S3 STA did not join the BL616CL SoftAP")
        client_ms, client_statuses = poll_paired_status(
            bridge, reference, 8, status_timeout, response_timeout
        )
        if client_ms is None:
            raise RuntimeError("BL616CL AP+STA did not observe its AP client")

        confirm_sta(bridge, reference, sta_ssid, response_timeout)

        require_exact(
            paired_transact(
                bridge, reference, b"AT+DISCONNECTSOFTAP\r\n", response_timeout
            ),
            TERMINAL,
            "BL616CL DISCONNECTSOFTAP",
        )
        ap_started = False
        require_exact(
            paired_transact(bridge, reference, b"AT+DISCONNECT\r\n", response_timeout),
            b"+DISCONNECT: \r\nOK\r\n",
            "BL616CL DISCONNECT",
        )
        sta_requested = False

        print(f"BL616CL_APSTA_STA_STATUSES={','.join(map(str, sta_statuses))}")
        print(f"BL616CL_APSTA_STA_CONNECTED_MS={sta_ms:.3f}")
        print(f"BL616CL_APSTA_AP_STATUSES={','.join(map(str, ap_statuses))}")
        print(f"BL616CL_APSTA_AP_STARTED_MS={ap_ms:.3f}")
        print(f"ESP32_APSTA_CLIENT_STATUSES={','.join(map(str, peer_statuses))}")
        print(f"ESP32_APSTA_CLIENT_CONNECTED_MS={peer_ms:.3f}")
        print(f"BL616CL_APSTA_CLIENT_STATUSES={','.join(map(str, client_statuses))}")
        print(f"BL616CL_APSTA_CLIENT_MS={client_ms:.3f}")
        print("BL616CL_APSTA_GATE_PASS")
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as error:
        primary_error = error
    finally:
        if bridge is not None and reference is not None and bridge.is_open and reference.is_open:
            if ap_started:
                try:
                    paired_transact(
                        bridge, reference, b"AT+DISCONNECTSOFTAP\r\n", response_timeout
                    )
                    print("BL616CL_APSTA_CLEANUP_AP_STOP_SENT")
                except (OSError, RuntimeError, serial.SerialException):
                    print("BL616CL_APSTA_CLEANUP_AP_STOP_FAILED", file=sys.stderr)
            if sta_requested:
                try:
                    paired_transact(bridge, reference, b"AT+DISCONNECT\r\n", response_timeout)
                    print("BL616CL_APSTA_CLEANUP_STA_DISCONNECT_SENT")
                except (OSError, RuntimeError, serial.SerialException):
                    print("BL616CL_APSTA_CLEANUP_STA_DISCONNECT_FAILED", file=sys.stderr)
            if wifi_mode_changed:
                try:
                    require_exact(
                        paired_transact(
                            bridge, reference, b"AT+WIFIMODE=1\r\n", response_timeout
                        ),
                        b"+WIFIMODE: \r\nOK\r\n",
                        "cleanup WIFIMODE=1",
                    )
                    print("BL616CL_APSTA_CLEANUP_MODE_STA_RESTORED")
                except (OSError, RuntimeError, serial.SerialException):
                    print("BL616CL_APSTA_CLEANUP_MODE_STA_RESTORE_FAILED", file=sys.stderr)
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
            ):
                if primary_error is None:
                    primary_error = RuntimeError("RA4M1 scan Sketch restore failed")
                else:
                    print("RA4_SCAN_RESTORE_FAILED", file=sys.stderr)

    if primary_error is not None:
        if sta_statuses:
            print(f"BL616CL_APSTA_STA_FAILURE_STATUSES={','.join(map(str, sta_statuses))}")
        if ap_statuses:
            print(f"BL616CL_APSTA_AP_FAILURE_STATUSES={','.join(map(str, ap_statuses))}")
        if peer_statuses:
            print(f"ESP32_APSTA_CLIENT_FAILURE_STATUSES={','.join(map(str, peer_statuses))}")
        if client_statuses:
            print(
                "BL616CL_APSTA_CLIENT_FAILURE_STATUSES="
                + ",".join(map(str, client_statuses))
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
