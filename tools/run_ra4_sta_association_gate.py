#!/usr/bin/env python3
"""Run a credential-safe paired-board BL616CL STA association gate.

Credentials are accepted only from the current process environment and are
never printed, written, or included in exception messages. The runner uses
the temporary RA4M1 AT proxy for the test, then restores the WiFiS3 scan smoke
Sketch unless --keep-proxy is explicitly requested.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import serial

from ra4_runtime_uart_route import open_runtime_route
from uno_r4_usb_identity import resolve_paired_bl616cl_port, resolve_relay_port


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_AT_GATE = ROOT_DIR / "tools" / "run_ra4_at_uart_gate.py"
DEFAULT_SCAN_SMOKE = ROOT_DIR / "tools" / "run_ra4_wifis3_scan_smoke.py"

WIFI_MODE_REQUEST = b"AT+WIFIMODE?\r\n"
WIFI_MODE_STA_RESPONSE = b"+WIFIMODE: 1\r\n\r\nOK\r\n"
RECONNECT_READ_REQUEST = b"AT+RECONNECT?\r\n"
RECONNECT_DISABLE_REQUEST = b"AT+RECONNECT=0\r\n"
RECONNECT_WRITE_RESPONSE = b"OK\r\n"
STATUS_REQUEST = b"AT+GETSTATUS?\r\n"
DISCONNECT_REQUEST = b"AT+DISCONNECT\r\n"
DISCONNECT_RESPONSE = b"+DISCONNECT: \r\nOK\r\n"
TERMINAL = b"OK\r\n"

STATUS_RESPONSE_PATTERN = re.compile(rb"^\+GETSTATUS: ([0-9]{1,3})\r\nOK\r\n$")
RECONNECT_RESPONSE_PATTERN = re.compile(rb"^\+RECONNECT: ([01])\r\nOK\r\n$")
BEGINSTA_RESPONSE_PATTERN = re.compile(rb"^\+BEGINSTA: ([0-9]{1,3})\r\nOK\r\n$")
MAXIMUM_IDEMPOTENT_ATTEMPTS = 3
IDEMPOTENT_RETRY_DELAY_SECONDS = 0.25


def require_port_identity(port: str, vendor: str, product: str, interface: str) -> None:
    device_path = Path(port)
    if not device_path.exists():
        raise RuntimeError(f"serial node does not exist: {port}")

    result = subprocess.run(
        [
            "udevadm",
            "info",
            "--query=property",
            f"--path=/sys/class/tty/{device_path.name}",
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"cannot inspect USB identity for {port}: {result.stderr.strip()}")

    properties = dict(
        line.split("=", 1) for line in result.stdout.splitlines() if "=" in line
    )
    actual_vendor = properties.get("ID_VENDOR_ID", "").lower()
    actual_product = properties.get("ID_MODEL_ID", "").lower()
    actual_interface = properties.get("ID_USB_INTERFACE_NUM", "").lower()
    if actual_vendor != vendor.lower() or actual_product != product.lower():
        raise RuntimeError(
            f"{port} has unexpected USB identity: "
            f"{actual_vendor or '<missing>'}:{actual_product or '<missing>'}; "
            f"expected {vendor}:{product}"
        )
    if actual_interface != interface.lower():
        raise RuntimeError(
            f"{port} has unexpected USB interface: "
            f"{actual_interface or '<missing>'}; expected {interface}"
        )


def open_serial(port: str, baudrate: int, timeout: float) -> serial.Serial:
    link = serial.Serial(
        port,
        baudrate=baudrate,
        timeout=timeout,
        write_timeout=timeout,
    )
    link.dtr = False
    link.rts = False
    return link


def require_ascii_credential(
    variable_name: str,
    value: str | None,
    maximum_bytes: int,
    required: bool,
) -> str | None:
    if value is None:
        if required:
            raise RuntimeError(f"{variable_name} must be set in the current environment")
        return None
    if not value:
        raise RuntimeError(f"{variable_name} must not be empty when set")
    try:
        encoded = value.encode("ascii", "strict")
    except UnicodeEncodeError as error:
        raise RuntimeError(f"{variable_name} must contain ASCII bytes only") from error
    if len(encoded) > maximum_bytes:
        raise RuntimeError(f"{variable_name} exceeds the {maximum_bytes}-byte limit")
    if any(byte in (0x00, 0x0A, 0x0D) for byte in encoded):
        raise RuntimeError(f"{variable_name} contains a forbidden control byte")
    return value


def quote_at_argument(value: str) -> str:
    """Quote an ASCII argument for the bridge parser without exposing it."""
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def build_beginsta_request(ssid: str, password: str | None) -> bytes:
    arguments = [quote_at_argument(ssid)]
    if password is not None:
        arguments.append(quote_at_argument(password))
    return ("AT+BEGINSTA=" + ",".join(arguments) + "\r\n").encode("ascii")


def read_until_terminal(
    link: serial.Serial,
    timeout_seconds: float,
    maximum_bytes: int,
) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        remaining = maximum_bytes - len(received)
        if remaining <= 0:
            raise RuntimeError(f"AT response exceeded {maximum_bytes} byte limit")
        payload = link.read(min(remaining, 256))
        if payload:
            received.extend(payload)
            if received.endswith(TERMINAL):
                return bytes(received)
    raise RuntimeError(
        f"AT response timed out after {timeout_seconds:.1f} seconds "
        f"with {len(received)} byte(s)"
    )


def transact(
    bridge: serial.Serial,
    reference: serial.Serial,
    request: bytes,
    timeout_seconds: float,
    maximum_bytes: int = 256,
) -> bytes:
    reference.reset_input_buffer()
    bridge.write(request)
    bridge.flush()
    return read_until_terminal(reference, timeout_seconds, maximum_bytes)


def transact_idempotent(
    bridge: serial.Serial,
    reference: serial.Serial,
    request: bytes,
    timeout_seconds: float,
    maximum_bytes: int = 256,
) -> bytes:
    """Retry only a query or setting that can safely be applied repeatedly."""
    last_error: RuntimeError | None = None
    response = b""
    for attempt in range(MAXIMUM_IDEMPOTENT_ATTEMPTS):
        try:
            response = transact(
                bridge,
                reference,
                request,
                timeout_seconds,
                maximum_bytes,
            )
        except RuntimeError as error:
            last_error = error
        else:
            return response
        if attempt + 1 < MAXIMUM_IDEMPOTENT_ATTEMPTS:
            time.sleep(IDEMPOTENT_RETRY_DELAY_SECONDS)
    assert last_error is not None
    raise last_error


def parse_reconnect_response(response: bytes) -> bool:
    match = RECONNECT_RESPONSE_PATTERN.fullmatch(response)
    if match is None:
        raise RuntimeError("RECONNECT readback did not match the expected AT framing")
    return match.group(1) == b"1"


def parse_status_response(response: bytes) -> int:
    match = STATUS_RESPONSE_PATTERN.fullmatch(response)
    if match is None:
        raise RuntimeError("GETSTATUS response did not match the expected AT framing")
    status = int(match.group(1))
    if status > 255:
        raise RuntimeError("GETSTATUS response contained an out-of-range value")
    return status


def validate_beginsta_response(response: bytes) -> None:
    match = BEGINSTA_RESPONSE_PATTERN.fullmatch(response)
    if match is None or int(match.group(1)) > 255:
        raise RuntimeError("BEGINSTA immediate response did not match the expected AT framing")


def require_exact_response(response: bytes, expected: bytes, label: str) -> None:
    if response != expected:
        raise RuntimeError(f"{label} did not match the expected AT framing")


def poll_for_status(
    bridge: serial.Serial,
    reference: serial.Serial,
    expected_status: int,
    timeout_seconds: float,
    response_timeout_seconds: float,
    poll_interval_seconds: float,
) -> tuple[float | None, list[int]]:
    started_at = time.monotonic()
    deadline = started_at + timeout_seconds
    observed: list[int] = []
    while time.monotonic() < deadline:
        response = transact(
            bridge,
            reference,
            STATUS_REQUEST,
            response_timeout_seconds,
        )
        status = parse_status_response(response)
        if not observed or observed[-1] != status:
            observed.append(status)
        if status == expected_status:
            return ((time.monotonic() - started_at) * 1000.0, observed)
        time.sleep(poll_interval_seconds)
    return (None, observed)


def run_proxy_upload(args: argparse.Namespace) -> None:
    command = [
        sys.executable,
        str(DEFAULT_AT_GATE),
        "--bridge-port",
        args.bridge_port,
        "--relay-port",
        args.relay_port,
        "--arduino-cli",
        args.arduino_cli,
        "--runtime-settle-ms",
        str(args.runtime_settle_ms),
        "--skip-status-check",
    ]
    print("COMMAND=" + " ".join(command))
    result = subprocess.run(command, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"RA4M1 proxy upload gate failed with exit status {result.returncode}")


def restore_scan_smoke(args: argparse.Namespace) -> None:
    command = [
        sys.executable,
        str(DEFAULT_SCAN_SMOKE),
        "--bridge-port",
        args.bridge_port,
        "--relay-port",
        args.relay_port,
        "--arduino-cli",
        args.arduino_cli,
    ]
    print("RA4_SCAN_RESTORE_STARTED")
    result = subprocess.run(command, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"RA4M1 scan Sketch restore failed with exit status {result.returncode}")
    print("RA4_SCAN_RESTORE_PASS")


def best_effort_cleanup(
    bridge: serial.Serial,
    reference: serial.Serial,
    original_reconnect_enabled: bool | None,
    response_timeout_seconds: float,
    association_requested: bool,
    disconnect_confirmed: bool,
) -> None:
    if association_requested and not disconnect_confirmed:
        try:
            response = transact(
                bridge,
                reference,
                DISCONNECT_REQUEST,
                response_timeout_seconds,
            )
            require_exact_response(response, DISCONNECT_RESPONSE, "cleanup DISCONNECT")
            print("STA_ASSOCIATION_CLEANUP_DISCONNECT_SENT")
        except (OSError, RuntimeError, serial.SerialException):
            print("STA_ASSOCIATION_CLEANUP_DISCONNECT_FAILED", file=sys.stderr)

    if original_reconnect_enabled is not None:
        request = b"AT+RECONNECT=1\r\n" if original_reconnect_enabled else b"AT+RECONNECT=0\r\n"
        try:
            response = transact(
                bridge,
                reference,
                request,
                response_timeout_seconds,
            )
            require_exact_response(response, RECONNECT_WRITE_RESPONSE, "cleanup RECONNECT")
            print("STA_ASSOCIATION_CLEANUP_RECONNECT_RESTORED")
        except (OSError, RuntimeError, serial.SerialException):
            print("STA_ASSOCIATION_CLEANUP_RECONNECT_RESTORE_FAILED", file=sys.stderr)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run a redacted paired-board BL616CL STA association/disconnect gate."
    )
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--relay-port", default="auto")
    parser.add_argument(
        "--reference-port",
        dest="relay_port",
        default=argparse.SUPPRESS,
        help=argparse.SUPPRESS,
    )
    parser.add_argument("--legacy-reference-port")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--response-timeout-ms", type=int, default=2000)
    parser.add_argument("--connect-timeout-ms", type=int, default=30000)
    parser.add_argument("--disconnect-timeout-ms", type=int, default=10000)
    parser.add_argument("--poll-interval-ms", type=int, default=250)
    parser.add_argument("--runtime-settle-ms", type=int, default=2000)
    parser.add_argument(
        "--upload-proxy",
        action="store_true",
        help="Build and upload the temporary RA4M1 Serial <-> Serial2 proxy first.",
    )
    parser.add_argument(
        "--keep-proxy",
        action="store_true",
        help="Do not restore ra4-wifis3-scan-smoke after an AT proxy session.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    args.relay_port = resolve_relay_port(args.relay_port)
    if (
        args.response_timeout_ms <= 0
        or args.connect_timeout_ms <= 0
        or args.disconnect_timeout_ms <= 0
        or args.poll_interval_ms <= 0
        or args.runtime_settle_ms < 0
    ):
        raise RuntimeError("timeouts must be positive")

    ssid = require_ascii_credential(
        "BL616CL_STA_SSID",
        os.environ.get("BL616CL_STA_SSID"),
        32,
        required=True,
    )
    password = require_ascii_credential(
        "BL616CL_STA_PASSWORD",
        os.environ.get("BL616CL_STA_PASSWORD"),
        64,
        required=False,
    )
    assert ssid is not None
    beginsta_request = build_beginsta_request(ssid, password)

    require_port_identity(args.bridge_port, "2341", "1002", "00")
    require_port_identity(args.relay_port, "0403", "6010", "00")
    if args.legacy_reference_port is not None:
        require_port_identity(args.legacy_reference_port, "0403", "6010", "00")
    print(f"Bridge:    {args.bridge_port} (2341:1002 interface 00)")
    print(f"Relay:     {args.relay_port} (0403:6010 interface 00)")
    print("Target: RA4M1 application Flash only when --upload-proxy or scan restoration runs.")
    print("Target: BL616CL Flash is untouched.")
    print("Relay route: RA4M1 UART (RTS=False, physical RTS# HIGH, relay released).")
    print("Privacy: credentials, BEGINSTA request bytes, and raw responses stay in memory.")

    bridge: serial.Serial | None = None
    reference: serial.Serial | None = None
    route = None
    original_reconnect_enabled: bool | None = None
    association_requested = False
    disconnect_confirmed = False
    proxy_active = False
    connected_elapsed_ms: float | None = None
    disconnected_elapsed_ms: float | None = None
    connected_statuses: list[int] = []
    disconnected_statuses: list[int] = []
    primary_error: Exception | None = None
    response_timeout_seconds = args.response_timeout_ms / 1000.0

    try:
        if args.upload_proxy:
            run_proxy_upload(args)
            proxy_active = True

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

        response = transact_idempotent(
            bridge,
            reference,
            WIFI_MODE_REQUEST,
            response_timeout_seconds,
        )
        require_exact_response(response, WIFI_MODE_STA_RESPONSE, "WIFIMODE preflight")
        proxy_active = True

        original_reconnect_enabled = parse_reconnect_response(
            transact_idempotent(
                bridge,
                reference,
                RECONNECT_READ_REQUEST,
                response_timeout_seconds,
            )
        )
        response = transact_idempotent(
            bridge,
            reference,
            RECONNECT_DISABLE_REQUEST,
            response_timeout_seconds,
        )
        require_exact_response(response, RECONNECT_WRITE_RESPONSE, "RECONNECT disable")
        print("STA_ASSOCIATION_RECONNECT_DISABLED")

        association_requested = True
        response = transact(
            bridge,
            reference,
            beginsta_request,
            response_timeout_seconds,
        )
        validate_beginsta_response(response)
        print("STA_ASSOCIATION_REQUEST_SENT")

        connected_elapsed_ms, connected_statuses = poll_for_status(
            bridge,
            reference,
            expected_status=3,
            timeout_seconds=args.connect_timeout_ms / 1000.0,
            response_timeout_seconds=response_timeout_seconds,
            poll_interval_seconds=args.poll_interval_ms / 1000.0,
        )
        if connected_elapsed_ms is None:
            raise RuntimeError("GETSTATUS did not reach 3 during STA association")

        response = transact(
            bridge,
            reference,
            DISCONNECT_REQUEST,
            response_timeout_seconds,
        )
        require_exact_response(response, DISCONNECT_RESPONSE, "DISCONNECT")
        disconnected_elapsed_ms, disconnected_statuses = poll_for_status(
            bridge,
            reference,
            expected_status=0,
            timeout_seconds=args.disconnect_timeout_ms / 1000.0,
            response_timeout_seconds=response_timeout_seconds,
            poll_interval_seconds=args.poll_interval_ms / 1000.0,
        )
        if disconnected_elapsed_ms is None:
            raise RuntimeError("GETSTATUS did not return to 0 after DISCONNECT")
        disconnect_confirmed = True
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        primary_error = error
    finally:
        if bridge is not None and reference is not None:
            best_effort_cleanup(
                bridge,
                reference,
                original_reconnect_enabled,
                response_timeout_seconds,
                association_requested,
                disconnect_confirmed,
            )
        if route is not None:
            route.close()

        if proxy_active and not args.keep_proxy:
            try:
                restore_scan_smoke(args)
            except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
                if primary_error is None:
                    primary_error = error
                else:
                    print("RA4_SCAN_RESTORE_FAILED", file=sys.stderr)

    if primary_error is not None:
        raise primary_error
    assert connected_elapsed_ms is not None
    assert disconnected_elapsed_ms is not None
    print(f"STA_ASSOCIATION_CONNECT_STATUSES={','.join(map(str, connected_statuses))}")
    print(f"STA_ASSOCIATION_CONNECTED_MS={connected_elapsed_ms:.3f}")
    print(f"STA_ASSOCIATION_DISCONNECT_STATUSES={','.join(map(str, disconnected_statuses))}")
    print(f"STA_ASSOCIATION_DISCONNECTED_MS={disconnected_elapsed_ms:.3f}")
    print("RA4_AT_STA_ASSOCIATION_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
