#!/usr/bin/env python3
"""Capture redacted ESP32-S3 STA success and authentication-failure behavior."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import serial

from uno_r4_usb_identity import (
    BASELINE_ESP32_SERIALS,
    resolve_baseline_esp32_port,
)

ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_FQBN = "arduino:renesas_uno:unor4wifi"
DEFAULT_PORT = "auto"
PROXY_SKETCH = ROOT_DIR / "examples" / "ra4-at-transcript-proxy"
PROXY_BUILD_DIR = ROOT_DIR / "build" / "esp32-ra4-at-transcript-proxy"
SCAN_SKETCH = ROOT_DIR / "examples" / "ra4-wifis3-scan-smoke"
SCAN_BUILD_DIR = ROOT_DIR / "build" / "esp32-ra4-wifis3-scan-smoke"

TERMINAL = b"OK\r\n"
WIFI_MODE_REQUEST = b"AT+WIFIMODE?\r\n"
WIFI_MODE_STA_RESPONSE = b"+WIFIMODE: 1\r\n\r\nOK\r\n"
RECONNECT_READ_REQUEST = b"AT+RECONNECT?\r\n"
RECONNECT_DISABLE_REQUEST = b"AT+RECONNECT=0\r\n"
RECONNECT_WRITE_RESPONSE = b"OK\r\n"
STATUS_REQUEST = b"AT+GETSTATUS?\r\n"
DISCONNECT_REQUEST = b"AT+DISCONNECT\r\n"
DISCONNECT_RESPONSE = b"+DISCONNECT: \r\nOK\r\n"
AT_REQUEST = b"AT\r\n"
AT_RESPONSE = b"OK\r\n"
FIRMWARE_REQUEST = b"AT+FWVERSION?\r\n"

STATUS_RESPONSE_PATTERN = re.compile(rb"^\+GETSTATUS: ([0-9]{1,3})\r\nOK\r\n$")
RECONNECT_RESPONSE_PATTERN = re.compile(rb"^\+RECONNECT: ([01])\r\nOK\r\n$")
BEGINSTA_RESPONSE_PATTERN = re.compile(rb"^\+BEGINSTA: ([0-9]{1,3})\r\nOK\r\n$")
FIRMWARE_RESPONSE_PATTERN = re.compile(
    rb"^\+FWVERSION: ([0-9]+\.[0-9]+\.[0-9]+)\r\nOK\r\n$"
)
SCAN_COUNT_PATTERN = re.compile(r"^RA4_WIFI_S3_SCAN:COUNT=(-?\d+)\r?$", re.MULTILINE)
SCAN_ELAPSED_PATTERN = re.compile(
    r"^RA4_WIFI_S3_SCAN:ELAPSED_MS=(\d+)\r?$", re.MULTILINE
)
SCAN_PASS_MARKER = "RA4_WIFI_S3_SCAN:RESULT=PASS"


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    result = subprocess.run(command, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"command failed with exit status {result.returncode}")


def require_port_identity(port: str) -> None:
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
    if (
        properties.get("ID_VENDOR_ID", "").lower() != "2341"
        or properties.get("ID_MODEL_ID", "").lower() != "1002"
        or properties.get("ID_USB_INTERFACE_NUM", "").lower() != "01"
        or properties.get("ID_SERIAL_SHORT") not in BASELINE_ESP32_SERIALS
    ):
        raise RuntimeError(
            f"{port} is not the expected original ESP32-S3 UNO R4 baseline board"
        )


def require_ascii_credential(
    variable_name: str,
    value: str | None,
    maximum_bytes: int,
) -> str:
    if not value:
        raise RuntimeError(f"{variable_name} must be set in the current environment")
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
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def build_beginsta_request(ssid: str, password: str) -> bytes:
    return (
        f"AT+BEGINSTA={quote_at_argument(ssid)},{quote_at_argument(password)}\r\n"
    ).encode("ascii")


def make_invalid_password(password: str) -> str:
    if len(password) < 64:
        return password + "!"
    replacement = "!" if password[-1] != "!" else "?"
    return password[:-1] + replacement


def open_serial(port: str, timeout: float) -> serial.Serial:
    link = serial.Serial(
        port=None,
        baudrate=115200,
        timeout=timeout,
        write_timeout=timeout,
    )
    # The original ESP32-S3 bridge forwards USB CDC data only while DTR is
    # asserted. Set the control lines before opening: toggling them after the
    # CDC node opens can leave the first proxied AT reply partially observed.
    link.dtr = True
    link.rts = False
    link.port = port
    link.open()
    return link


def read_until_terminal(
    link: serial.Serial, timeout_seconds: float, maximum_bytes: int = 256
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


def transact(link: serial.Serial, request: bytes, timeout_seconds: float) -> bytes:
    link.reset_input_buffer()
    link.write(request)
    link.flush()
    return read_until_terminal(link, timeout_seconds)


def require_exact(response: bytes, expected: bytes, label: str) -> None:
    if response != expected:
        raise RuntimeError(f"{label} did not match the expected ESP32-S3 framing")


def parse_status(response: bytes) -> int:
    match = STATUS_RESPONSE_PATTERN.fullmatch(response)
    if match is None:
        raise RuntimeError("GETSTATUS did not match the expected ESP32-S3 framing")
    value = int(match.group(1))
    if value > 255:
        raise RuntimeError("GETSTATUS returned an out-of-range status")
    return value


def parse_reconnect(response: bytes) -> bool:
    match = RECONNECT_RESPONSE_PATTERN.fullmatch(response)
    if match is None:
        raise RuntimeError("RECONNECT did not match the expected ESP32-S3 framing")
    return match.group(1) == b"1"


def validate_beginsta(response: bytes) -> None:
    match = BEGINSTA_RESPONSE_PATTERN.fullmatch(response)
    if match is None or int(match.group(1)) > 255:
        raise RuntimeError("BEGINSTA did not match the expected ESP32-S3 framing")


def parse_firmware(response: bytes) -> str:
    match = FIRMWARE_RESPONSE_PATTERN.fullmatch(response)
    if match is None:
        raise RuntimeError("FWVERSION did not match the expected ESP32-S3 framing")
    return match.group(1).decode("ascii")


def poll_status(
    link: serial.Serial,
    timeout_seconds: float,
    response_timeout_seconds: float,
    expected: int | None,
) -> tuple[float | None, list[int]]:
    started_at = time.monotonic()
    deadline = started_at + timeout_seconds
    observed: list[int] = []
    while time.monotonic() < deadline:
        status = parse_status(transact(link, STATUS_REQUEST, response_timeout_seconds))
        if not observed or observed[-1] != status:
            observed.append(status)
        if expected is not None and status == expected:
            return ((time.monotonic() - started_at) * 1000.0, observed)
        time.sleep(0.25)
    return (None, observed)


def compile_and_upload(
    args: argparse.Namespace, sketch: Path, build_dir: Path, label: str
) -> None:
    run_command(
        [
            args.arduino_cli,
            "compile",
            "--fqbn",
            args.fqbn,
            "--build-path",
            str(build_dir),
            str(sketch),
        ]
    )
    run_command(
        [
            args.arduino_cli,
            "upload",
            "--fqbn",
            args.fqbn,
            "--port",
            args.port,
            "--input-dir",
            str(build_dir),
        ]
    )
    print(f"{label}_UPLOAD_PASS")


def restore_scan_smoke(args: argparse.Namespace) -> None:
    print("ESP32_RA4_SCAN_RESTORE_STARTED")
    compile_and_upload(args, SCAN_SKETCH, SCAN_BUILD_DIR, "ESP32_RA4_SCAN")
    time.sleep(args.scan_startup_timeout_ms / 1000)

    link = open_serial(args.port, 0.1)
    try:
        deadline = time.monotonic() + args.scan_capture_timeout_ms / 1000
        received = bytearray()
        while time.monotonic() < deadline:
            payload = link.read(512)
            if payload:
                received.extend(payload)
                if len(received) > 8192:
                    raise RuntimeError("WiFiS3 scan restore capture exceeded 8192 bytes")
                runtime = received.decode("ascii", errors="replace").replace("\x00", "")
                if SCAN_PASS_MARKER in runtime:
                    count = SCAN_COUNT_PATTERN.search(runtime)
                    elapsed = SCAN_ELAPSED_PATTERN.search(runtime)
                    if count is None or elapsed is None or int(count.group(1)) <= 0:
                        raise RuntimeError("WiFiS3 scan restore markers were incomplete")
                    print(f"ESP32_RA4_SCAN_COUNT={count.group(1)}")
                    print(f"ESP32_RA4_SCAN_ELAPSED_MS={elapsed.group(1)}")
                    print("ESP32_RA4_SCAN_RESTORE_PASS")
                    return
        raise RuntimeError("WiFiS3 scan restore did not produce a PASS marker")
    finally:
        link.close()


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Capture redacted ESP32-S3 STA success and authentication-failure behavior."
    )
    parser.add_argument("--port", default=DEFAULT_PORT)
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--fqbn", default=DEFAULT_FQBN)
    parser.add_argument("--response-timeout-ms", type=int, default=2000)
    parser.add_argument("--connect-timeout-ms", type=int, default=30000)
    parser.add_argument("--failure-observe-ms", type=int, default=15000)
    parser.add_argument("--scan-startup-timeout-ms", type=int, default=100)
    parser.add_argument("--scan-capture-timeout-ms", type=int, default=45000)
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    args.port = resolve_baseline_esp32_port(args.port)
    if (
        args.response_timeout_ms <= 0
        or args.connect_timeout_ms <= 0
        or args.failure_observe_ms <= 0
        or args.scan_startup_timeout_ms < 0
        or args.scan_capture_timeout_ms <= 0
    ):
        raise RuntimeError("invalid timeout")

    ssid = require_ascii_credential("BL616CL_STA_SSID", os.environ.get("BL616CL_STA_SSID"), 32)
    password = require_ascii_credential(
        "BL616CL_STA_PASSWORD", os.environ.get("BL616CL_STA_PASSWORD"), 64
    )
    success_request = build_beginsta_request(ssid, password)
    failure_request = build_beginsta_request(ssid, make_invalid_password(password))
    response_timeout_seconds = args.response_timeout_ms / 1000

    require_port_identity(args.port)
    print(f"ESP32_BASELINE_PORT={args.port}")
    print("Target: original ESP32-S3 UNO R4 RA4M1 application Flash only.")
    print("Target: ESP32-S3 firmware Flash is untouched.")
    print("Privacy: credentials, BEGINSTA request bytes, and raw AT responses stay in memory.")

    link: serial.Serial | None = None
    original_reconnect_enabled: bool | None = None
    association_requested = False
    disconnected = False
    proxy_uploaded = False
    primary_error: Exception | None = None
    try:
        compile_and_upload(args, PROXY_SKETCH, PROXY_BUILD_DIR, "ESP32_RA4_PROXY")
        proxy_uploaded = True
        time.sleep(2.0)

        link = open_serial(args.port, 0.1)
        require_exact(transact(link, AT_REQUEST, response_timeout_seconds), AT_RESPONSE, "AT")
        firmware = parse_firmware(
            transact(link, FIRMWARE_REQUEST, response_timeout_seconds)
        )
        print(f"ESP32_STA_FIRMWARE={firmware}")
        require_exact(
            transact(link, WIFI_MODE_REQUEST, response_timeout_seconds),
            WIFI_MODE_STA_RESPONSE,
            "WIFIMODE",
        )
        original_reconnect_enabled = parse_reconnect(
            transact(link, RECONNECT_READ_REQUEST, response_timeout_seconds)
        )
        require_exact(
            transact(link, RECONNECT_DISABLE_REQUEST, response_timeout_seconds),
            RECONNECT_WRITE_RESPONSE,
            "RECONNECT disable",
        )
        print("ESP32_STA_RECONNECT_DISABLED")

        association_requested = True
        validate_beginsta(transact(link, success_request, response_timeout_seconds))
        connected_ms, connected_statuses = poll_status(
            link,
            args.connect_timeout_ms / 1000,
            response_timeout_seconds,
            expected=3,
        )
        print(f"ESP32_STA_SUCCESS_STATUSES={','.join(map(str, connected_statuses))}")
        if connected_ms is None:
            raise RuntimeError("ESP32-S3 baseline did not reach GETSTATUS=3")
        print(f"ESP32_STA_SUCCESS_CONNECTED_MS={connected_ms:.3f}")

        require_exact(
            transact(link, DISCONNECT_REQUEST, response_timeout_seconds),
            DISCONNECT_RESPONSE,
            "DISCONNECT after successful association",
        )
        disconnected_ms, disconnected_statuses = poll_status(
            link,
            args.connect_timeout_ms / 1000,
            response_timeout_seconds,
            expected=0,
        )
        if disconnected_ms is None:
            raise RuntimeError("ESP32-S3 baseline did not return to GETSTATUS=0")
        disconnected = True
        print(f"ESP32_STA_DISCONNECT_STATUSES={','.join(map(str, disconnected_statuses))}")
        print(f"ESP32_STA_DISCONNECTED_MS={disconnected_ms:.3f}")

        validate_beginsta(transact(link, failure_request, response_timeout_seconds))
        _, failure_statuses = poll_status(
            link,
            args.failure_observe_ms / 1000,
            response_timeout_seconds,
            expected=None,
        )
        if 3 in failure_statuses:
            raise RuntimeError("authentication-failure probe unexpectedly reached GETSTATUS=3")
        print(f"ESP32_STA_AUTH_FAILURE_STATUSES={','.join(map(str, failure_statuses))}")
        print("ESP32_STA_AUTH_FAILURE_NO_CONNECT")
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        primary_error = error
    finally:
        if link is not None and link.is_open:
            if association_requested and not disconnected:
                try:
                    require_exact(
                        transact(link, DISCONNECT_REQUEST, response_timeout_seconds),
                        DISCONNECT_RESPONSE,
                        "cleanup DISCONNECT",
                    )
                    print("ESP32_STA_CLEANUP_DISCONNECT_SENT")
                except (OSError, RuntimeError, serial.SerialException):
                    print("ESP32_STA_CLEANUP_DISCONNECT_FAILED", file=sys.stderr)
            if original_reconnect_enabled is not None:
                request = (
                    b"AT+RECONNECT=1\r\n"
                    if original_reconnect_enabled
                    else b"AT+RECONNECT=0\r\n"
                )
                try:
                    require_exact(
                        transact(link, request, response_timeout_seconds),
                        RECONNECT_WRITE_RESPONSE,
                        "cleanup RECONNECT",
                    )
                    print("ESP32_STA_CLEANUP_RECONNECT_RESTORED")
                except (OSError, RuntimeError, serial.SerialException):
                    print("ESP32_STA_CLEANUP_RECONNECT_RESTORE_FAILED", file=sys.stderr)
            link.close()

        if proxy_uploaded:
            try:
                restore_scan_smoke(args)
            except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
                if primary_error is None:
                    primary_error = error
                else:
                    print("ESP32_RA4_SCAN_RESTORE_FAILED", file=sys.stderr)

    if primary_error is not None:
        raise primary_error
    print("ESP32_STA_BASELINE_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
