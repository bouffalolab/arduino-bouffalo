#!/usr/bin/env python3
"""Validate BL616CL UDP through a two-board WiFiS3 UDP echo."""

from __future__ import annotations

import argparse
import re
import secrets
import subprocess
import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_esp32_sta_baseline_gate as baseline
import run_ra4_wifis3_scan_smoke as paired_scan
from ra4_runtime_uart_route import open_runtime_route
from uno_r4_usb_identity import (
    resolve_baseline_esp32_port,
    resolve_paired_bl616cl_port,
    resolve_relay_port,
)


ROOT_DIR = Path(__file__).resolve().parents[1]
FQBN = "arduino:renesas_uno:unor4wifi"
FLASH_RUNNER = ROOT_DIR / "tools" / "run_bl616cl_paired_flash.py"
WINDOWS_DIRECT_UPLOADER = ROOT_DIR / "tools" / "upload_ra4_samba_windows_direct.py"
DIRECT_RESET_RUNNER = ROOT_DIR / "tools" / "reset_ra4_samba_direct.py"
ECHO_SKETCH = ROOT_DIR / "examples" / "ra4-wifis3-softap-udp-echo"
ECHO_BUILD = ROOT_DIR / "build" / "ra4-wifis3-softap-udp-echo"
CLIENT_SKETCH = ROOT_DIR / "examples" / "ra4-wifis3-udp-client-smoke"
CLIENT_BUILD = ROOT_DIR / "build" / "ra4-wifis3-udp-client-smoke"

ECHO_READY = "RA4_UDP_ECHO:READY"
ECHO_SELFTEST = "RA4_UDP_ECHO:SELFTEST_PASS"
ECHO_ADDRESS_PATTERN = re.compile(r"^RA4_UDP_ECHO:ADDRESS=(\d{1,3}(?:\.\d{1,3}){3})\r?$", re.MULTILINE)
ECHO_PASS = "RA4_UDP_ECHO:RESULT=PASS"
ECHO_FAIL = "RA4_UDP_ECHO:RESULT=FAIL:"
CLIENT_PASS = "RA4_UDP_CLIENT:RESULT=PASS"
CLIENT_FAIL = "RA4_UDP_CLIENT:RESULT=FAIL:"


class MarkerCaptureError(RuntimeError):
    def __init__(self, marker: str, captured: str) -> None:
        super().__init__(f"RA4M1 UDP smoke reported failure: {marker}")
        self.captured = captured


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    result = subprocess.run(command, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"subprocess failed with exit status {result.returncode}")


def open_serial(port: str, timeout: float) -> serial.Serial:
    link = serial.Serial(port, 115200, timeout=timeout, write_timeout=timeout)
    link.dtr = True
    link.rts = False
    return link


def capture_until(
    link: serial.Serial,
    required: tuple[str, ...],
    failure_prefix: str,
    timeout_seconds: float,
) -> str:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        chunk = link.read(512)
        if not chunk:
            continue
        received.extend(chunk)
        if len(received) > 8192:
            raise RuntimeError("serial marker capture exceeded its byte limit")
        text = received.decode("ascii", errors="replace").replace("\x00", "")
        if failure_prefix in text:
            start = text.find(failure_prefix)
            end = text.find("\n", start)
            marker = text[start:end if end >= 0 else len(text)].strip()
            raise MarkerCaptureError(marker, text)
        if all(marker in text for marker in required):
            return text
    raise RuntimeError("required RA4M1 UDP markers were not observed")


def print_markers(label: str, text: str, prefix: str) -> None:
    markers = [
        line.strip()
        for line in text.replace("\x00", "").splitlines()
        if line.strip().startswith(prefix)
    ]
    if markers:
        print(f"{label}_BEGIN")
        for marker in markers:
            print(marker)
        print(f"{label}_END")

def extract_echo_address(text: str) -> str:
    match = ECHO_ADDRESS_PATTERN.search(text.replace("\x00", ""))
    if match is None:
        raise RuntimeError("UDP echo peer did not report a usable SoftAP IPv4 address")
    octets = match.group(1).split(".")
    if any(int(octet) > 255 for octet in octets):
        raise RuntimeError("UDP echo peer reported an invalid SoftAP IPv4 address")
    return match.group(1)


def compile_sketch(args: argparse.Namespace, sketch: Path, build: Path) -> Path:
    run_command(
        [
            args.arduino_cli,
            "compile",
            "--fqbn",
            FQBN,
            "--build-path",
            str(build),
            str(sketch),
        ]
    )
    image = build / f"{sketch.name}.ino.bin"
    if not image.is_file():
        raise RuntimeError(f"compiled image does not exist: {image}")
    return image


def upload_paired(args: argparse.Namespace, image: Path) -> None:
    run_command(
        [
            sys.executable,
            str(WINDOWS_DIRECT_UPLOADER),
            "--bridge-port",
            args.bridge_port,
            "--relay-port",
            args.relay_port,
            "--image",
            str(image),
            "--runtime-settle-ms",
            str(args.runtime_settle_ms),
        ]
    )


def preflight_paired_upload_path(args: argparse.Namespace) -> None:
    run_command(
        [
            sys.executable,
            str(DIRECT_RESET_RUNNER),
            "--bridge-port",
            args.bridge_port,
            "--relay-port",
            args.relay_port,
            "--attempts",
            "1",
            "--capture-ms",
            "1000",
        ]
    )


def restore_paired(args: argparse.Namespace) -> None:
    run_command(
        [
            sys.executable,
            str(paired_scan.__file__),
            "--bridge-port",
            args.bridge_port,
            "--relay-port",
            args.relay_port,
            "--arduino-cli",
            args.arduino_cli,
        ]
    )
    print("PAIRED_RA4_SCAN_RESTORE_PASS")


def restore_baseline(args: argparse.Namespace) -> None:
    baseline.restore_scan_smoke(
        argparse.Namespace(
            arduino_cli=args.arduino_cli,
            fqbn=FQBN,
            port=args.esp32_port,
            scan_startup_timeout_ms=100,
            scan_capture_timeout_ms=45000,
        )
    )
    print("ESP32_RA4_SCAN_RESTORE_PASS")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--relay-port", default="auto")
    parser.add_argument("--esp32-port", default="auto")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--ready-timeout-ms", type=int, default=30000)
    parser.add_argument("--client-timeout-ms", type=int, default=60000)
    parser.add_argument("--runtime-settle-ms", type=int, default=2000)
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    args.relay_port = resolve_relay_port(args.relay_port)
    args.esp32_port = resolve_baseline_esp32_port(args.esp32_port)
    if (
        args.ready_timeout_ms <= 0
        or args.client_timeout_ms <= 0
        or args.runtime_settle_ms < 0
    ):
        raise RuntimeError("invalid timeout")

    baseline.require_port_identity(args.esp32_port)
    print(f"BL616CL bridge: {args.bridge_port} (2341:1002 interface 00)")
    print(f"Relay:           {args.relay_port} (0403:6010 interface 00)")
    print(f"ESP32-S3 peer:   {args.esp32_port} (2341:1002 original baseline)")
    print("Target: BL616CL UDP client path; ESP32-S3 is a temporary UDP peer.")
    print("Privacy: generated SoftAP credentials stay in memory.")

    ssid = "uno-r4-" + secrets.token_hex(4)
    password = secrets.token_urlsafe(18)
    echo: serial.Serial | None = None
    route = None
    paired_modified = False
    baseline_modified = False
    primary_error: Exception | None = None

    try:
        echo_image = compile_sketch(args, ECHO_SKETCH, ECHO_BUILD)
        run_command(
            [
                args.arduino_cli,
                "upload",
                "--fqbn",
                FQBN,
                "--port",
                args.esp32_port,
                "--input-dir",
                str(ECHO_BUILD),
            ]
        )
        baseline_modified = True
        echo = open_serial(args.esp32_port, 0.1)
        echo.reset_input_buffer()
        echo.write(f"START {ssid} {password}\n".encode("ascii"))
        echo.flush()
        echo_capture = capture_until(
            echo,
            (ECHO_READY, ECHO_SELFTEST, "RA4_UDP_ECHO:ADDRESS="),
            ECHO_FAIL,
            args.ready_timeout_ms / 1000.0,
        )
        print_markers("RA4_UDP_ECHO", echo_capture, "RA4_UDP_ECHO:")
        echo_address = extract_echo_address(echo_capture)

        run_command(
            [
                sys.executable,
                str(FLASH_RUNNER),
                "--bridge-port",
                args.bridge_port,
                "--relay-port",
                args.relay_port,
            ]
        )
        preflight_paired_upload_path(args)
        client_image = compile_sketch(args, CLIENT_SKETCH, CLIENT_BUILD)
        upload_paired(args, client_image)
        paired_modified = True

        route = open_runtime_route(
            args.bridge_port,
            args.relay_port,
            bridge_timeout=1.0,
            receiver_timeout=0.1,
        )
        route.bridge.reset_input_buffer()
        route.bridge.write(
            f"START {ssid} {password} {echo_address} {4244}\n".encode("ascii")
        )
        route.bridge.flush()
        client_capture = capture_until(
            route.receiver,
            (CLIENT_PASS,),
            CLIENT_FAIL,
            args.client_timeout_ms / 1000.0,
        )
        print_markers("RA4_UDP_CLIENT", client_capture, "RA4_UDP_CLIENT:")

        echo_capture = capture_until(
            echo,
            (ECHO_PASS,),
            ECHO_FAIL,
            args.client_timeout_ms / 1000.0,
        )
        print_markers("RA4_UDP_ECHO", echo_capture, "RA4_UDP_ECHO:")
        print("RA4_WIFIS3_UDP_ECHO_GATE_PASS")
    except MarkerCaptureError as error:
        print_markers("UDP_DIAGNOSTICS", error.captured, "RA4_UDP_")
        if echo is not None and echo.is_open:
            try:
                echo_capture = capture_until(
                    echo,
                    (ECHO_PASS,),
                    ECHO_FAIL,
                    2.0,
                )
                print_markers("RA4_UDP_ECHO", echo_capture, "RA4_UDP_ECHO:")
            except (MarkerCaptureError, OSError, RuntimeError, serial.SerialException):
                pass
        primary_error = error
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        primary_error = error
    finally:
        if route is not None:
            route.close()
        if echo is not None and echo.is_open:
            echo.close()
        if paired_modified:
            try:
                restore_paired(args)
            except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError):
                print("PAIRED_RA4_SCAN_RESTORE_FAILED", file=sys.stderr)
                if primary_error is None:
                    primary_error = RuntimeError("paired RA4M1 scan restore failed")
        if baseline_modified:
            try:
                restore_baseline(args)
            except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError):
                print("ESP32_RA4_SCAN_RESTORE_FAILED", file=sys.stderr)
                if primary_error is None:
                    primary_error = RuntimeError("ESP32 RA4M1 scan restore failed")

    if primary_error is not None:
        raise primary_error
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
