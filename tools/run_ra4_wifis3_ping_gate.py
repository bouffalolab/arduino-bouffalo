#!/usr/bin/env python3
"""Validate BL616CL WiFiS3 ping through an isolated ESP32-S3 SoftAP.

The original ESP32-S3 UNO R4 temporarily provides only the SoftAP peer. The
paired RA4M1 verifies ICMP echo plus unknown-host and timeout result mapping.
Generated SoftAP credentials stay in process memory. Both RA4M1 boards are
restored to scan smoke on all normal exit paths.
"""

from __future__ import annotations

import argparse
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
BOSSAC_RUNNER = ROOT_DIR / "tools" / "run_ra4_bossac_direct.py"
SOFTAP_SKETCH = ROOT_DIR / "examples" / "ra4-wifis3-softap-tcp-echo"
SOFTAP_BUILD = ROOT_DIR / "build" / "ra4-wifis3-ping-softap"
PING_SKETCH = ROOT_DIR / "examples" / "ra4-wifis3-ping-smoke"
PING_BUILD = ROOT_DIR / "build" / "ra4-wifis3-ping-smoke"

SOFTAP_READY = "RA4_TCP_ECHO:READY"
SOFTAP_FAIL = "RA4_TCP_ECHO:RESULT=FAIL:"
PING_PASS = "RA4_PING:RESULT=PASS"
PING_FAIL = "RA4_PING:RESULT=FAIL:"


class MarkerCaptureError(RuntimeError):
    def __init__(self, marker: str, captured: str) -> None:
        super().__init__(f"RA4M1 ping smoke reported failure: {marker}")
        self.captured = captured


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def open_baseline_serial(port: str, timeout: float) -> serial.Serial:
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
        payload = link.read(512)
        if not payload:
            continue
        received.extend(payload)
        if len(received) > 8192:
            raise RuntimeError("serial marker capture exceeded its byte limit")
        text = received.decode("ascii", errors="replace").replace("\x00", "")
        if failure_prefix in text:
            marker_start = text.find(failure_prefix)
            marker_end = text.find("\n", marker_start)
            marker = text[marker_start:marker_end if marker_end >= 0 else len(text)].strip()
            raise MarkerCaptureError(marker, text)
        if all(marker in text for marker in required):
            return text
    raise RuntimeError("required RA4M1 smoke markers were not observed")


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


def upload_baseline_softap(args: argparse.Namespace, image: Path) -> None:
    run_command(
        [
            args.arduino_cli,
            "upload",
            "--fqbn",
            FQBN,
            "--port",
            args.esp32_port,
            "--input-dir",
            str(image.parent),
        ]
    )


def upload_paired_ping(args: argparse.Namespace, image: Path) -> None:
    run_command(
        [
            sys.executable,
            str(BOSSAC_RUNNER),
            "--bridge-port",
            args.bridge_port,
            "--relay-port",
            args.relay_port,
            "--image",
            str(image),
            "--runtime-settle-ms",
            str(args.paired_runtime_settle_ms),
            "--attempts",
            "3",
            "--skip-runtime-smoke",
        ]
    )


def restore_paired_scan(args: argparse.Namespace) -> None:
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


def restore_baseline_scan(args: argparse.Namespace) -> None:
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
    parser.add_argument("--softap-ready-timeout-ms", type=int, default=30000)
    parser.add_argument("--ping-timeout-ms", type=int, default=45000)
    parser.add_argument("--paired-runtime-settle-ms", type=int, default=2000)
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    args.relay_port = resolve_relay_port(args.relay_port)
    args.esp32_port = resolve_baseline_esp32_port(args.esp32_port)
    if (
        args.softap_ready_timeout_ms <= 0
        or args.ping_timeout_ms <= 0
        or args.paired_runtime_settle_ms < 0
    ):
        raise RuntimeError("invalid timeout")

    baseline.require_port_identity(args.esp32_port)
    print(f"BL616CL bridge: {args.bridge_port} (2341:1002 interface 00)")
    print(f"Relay:           {args.relay_port} (0403:6010 interface 00)")
    print(f"ESP32-S3 peer:   {args.esp32_port} (2341:1002 original baseline)")
    print("Target: BL616CL bridge Flash, then both RA4M1 application Flash areas.")
    print("Privacy: generated SoftAP credentials stay in memory.")

    ap_ssid = "uno-r4-" + secrets.token_hex(4)
    ap_password = secrets.token_urlsafe(18)
    baseline_link: serial.Serial | None = None
    route = None
    paired_modified = False
    baseline_modified = False
    primary_error: Exception | None = None

    try:
        softap_image = compile_sketch(args, SOFTAP_SKETCH, SOFTAP_BUILD)
        upload_baseline_softap(args, softap_image)
        baseline_modified = True
        baseline_link = open_baseline_serial(args.esp32_port, 0.1)
        baseline_link.reset_input_buffer()
        baseline_link.write(f"START {ap_ssid} {ap_password}\n".encode("ascii"))
        baseline_link.flush()
        softap_capture = capture_until(
            baseline_link,
            (SOFTAP_READY,),
            SOFTAP_FAIL,
            args.softap_ready_timeout_ms / 1000.0,
        )
        print_markers("ESP32_PING_SOFTAP", softap_capture, "RA4_TCP_ECHO:")

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
        ping_image = compile_sketch(args, PING_SKETCH, PING_BUILD)
        upload_paired_ping(args, ping_image)
        paired_modified = True

        route = open_runtime_route(
            args.bridge_port,
            args.relay_port,
            bridge_timeout=1.0,
            receiver_timeout=0.1,
        )
        route.bridge.reset_input_buffer()
        route.bridge.write(
            f"START {ap_ssid} {ap_password} 192.168.4.1\n".encode("ascii")
        )
        route.bridge.flush()
        ping_capture = capture_until(
            route.receiver,
            (
                "RA4_PING:WIFI_CONNECTED",
                "RA4_PING:SUCCESS_PASS",
                "RA4_PING:UNKNOWN_HOST_PASS",
                "RA4_PING:TIMEOUT_PASS",
                PING_PASS,
            ),
            PING_FAIL,
            args.ping_timeout_ms / 1000.0,
        )
        print_markers("PAIRED_PING", ping_capture, "RA4_PING:")
        print("RA4_WIFIS3_PING_GATE_PASS")
    except MarkerCaptureError as error:
        print_markers("RA4_PING_DIAGNOSTICS", error.captured, "RA4_PING:")
        primary_error = error
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        primary_error = error
    finally:
        if route is not None:
            route.close()
        if baseline_link is not None and baseline_link.is_open:
            baseline_link.close()
        if paired_modified:
            try:
                restore_paired_scan(args)
            except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError):
                print("PAIRED_RA4_SCAN_RESTORE_FAILED", file=sys.stderr)
                if primary_error is None:
                    primary_error = RuntimeError("paired RA4M1 scan restore failed")
        if baseline_modified:
            try:
                restore_baseline_scan(args)
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
