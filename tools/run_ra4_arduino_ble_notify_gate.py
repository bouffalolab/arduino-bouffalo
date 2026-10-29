#!/usr/bin/env python3
"""Run a two-UNO-R4 ArduinoBLE central/peripheral Notify gate and restore both boards."""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

import serial

import run_esp32_sta_baseline_gate as esp32
from uno_r4_usb_identity import (
    resolve_baseline_esp32_port,
    resolve_paired_bl616cl_port,
    resolve_relay_port,
)


ROOT_DIR = Path(__file__).resolve().parents[1]
FQBN = "arduino:renesas_uno:unor4wifi"

PERIPHERAL_SKETCH = ROOT_DIR / "examples" / "ra4-arduino-ble-notify-peripheral"
PERIPHERAL_BUILD_DIR = ROOT_DIR / "build" / "ra4-arduino-ble-notify-peripheral"
CENTRAL_SKETCH = ROOT_DIR / "examples" / "ra4-arduino-ble-notify-central"
CENTRAL_BUILD_DIR = ROOT_DIR / "build" / "ra4-arduino-ble-notify-central"
CENTRAL_IMAGE = CENTRAL_BUILD_DIR / "ra4-arduino-ble-notify-central.ino.bin"

DIRECT_BOSSAC_RUNNER = ROOT_DIR / "tools" / "run_ra4_bossac_direct.py"
DIRECT_RESET_RUNNER = ROOT_DIR / "tools" / "reset_ra4_samba_direct.py"
PAIRED_SCAN_RESTORE = ROOT_DIR / "tools" / "run_ra4_wifis3_scan_smoke.py"

PERIPHERAL_READY = "RA4_BLE_NOTIFY_PERIPHERAL:READY"
PERIPHERAL_MARKERS = (
    "RA4_BLE_NOTIFY_PERIPHERAL:SUBSCRIBED",
    "RA4_BLE_NOTIFY_PERIPHERAL:NOTIFY_SENT=1",
    "RA4_BLE_NOTIFY_PERIPHERAL:NOTIFY_SENT=2",
    "RA4_BLE_NOTIFY_PERIPHERAL:NOTIFY_SENT=3",
    "RA4_BLE_NOTIFY_PERIPHERAL:SUBSCRIPTION_CLEARED",
)
CENTRAL_MARKERS = (
    "RA4_BLE_NOTIFY_CENTRAL:BOOT",
    "RA4_BLE_NOTIFY_CENTRAL:BLE_BEGIN=PASS",
    "RA4_BLE_NOTIFY_CENTRAL:SCAN_START=PASS",
    "RA4_BLE_NOTIFY_CENTRAL:TARGET_FOUND",
    "RA4_BLE_NOTIFY_CENTRAL:CONNECT=PASS",
    "RA4_BLE_NOTIFY_CENTRAL:DISCOVER=PASS",
    "RA4_BLE_NOTIFY_CENTRAL:CHARACTERISTIC=PASS",
    "RA4_BLE_NOTIFY_CENTRAL:SUBSCRIBE=PASS",
    "RA4_BLE_NOTIFY_CENTRAL:NOTIFICATION=1:PASS",
    "RA4_BLE_NOTIFY_CENTRAL:NOTIFICATION=2:PASS",
    "RA4_BLE_NOTIFY_CENTRAL:NOTIFICATION=3:PASS",
    "RA4_BLE_NOTIFY_CENTRAL:UNSUBSCRIBE=PASS",
    "RA4_BLE_NOTIFY_CENTRAL:DISCONNECT=PASS",
    "RA4_BLE_NOTIFY_CENTRAL:BLE_END_CALLED",
    "RA4_BLE_NOTIFY_CENTRAL:DONE",
    "RA4_BLE_NOTIFY_CENTRAL:RESULT=PASS",
)


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def compile_sketch(arduino_cli: str, sketch: Path, build_dir: Path) -> None:
    run_command(
        [
            arduino_cli,
            "compile",
            "--fqbn",
            FQBN,
            "--libraries",
            str(ROOT_DIR / "libraries"),
            "--build-path",
            str(build_dir),
            str(sketch),
        ]
    )


def capture_until_markers(
    link: serial.Serial,
    markers: tuple[str, ...],
    timeout_seconds: float,
    maximum_bytes: int = 8192,
) -> str:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        payload = link.read(512)
        if payload:
            received.extend(payload)
            if len(received) > maximum_bytes:
                raise RuntimeError("serial capture exceeded its byte limit")
            text = received.decode("ascii", errors="replace").replace("\x00", "")
            if all(marker in text for marker in markers):
                return text
    return received.decode("ascii", errors="replace").replace("\x00", "")


def upload_baseline_peripheral(args: argparse.Namespace) -> serial.Serial:
    run_command(
        [
            args.arduino_cli,
            "upload",
            "--fqbn",
            FQBN,
            "--port",
            args.esp32_port,
            "--input-dir",
            str(PERIPHERAL_BUILD_DIR),
        ]
    )
    print("RA4_BLE_NOTIFY_PERIPHERAL_UPLOAD_PASS")

    time.sleep(args.peripheral_settle_ms / 1000.0)
    link = esp32.open_serial(args.esp32_port, 0.1)
    link.reset_input_buffer()
    runtime = capture_until_markers(
        link,
        (PERIPHERAL_READY,),
        args.peripheral_ready_timeout_ms / 1000.0,
    )
    if PERIPHERAL_READY not in runtime:
        link.close()
        raise RuntimeError("ESP32-S3 RA4M1 peripheral did not report ready")
    print("RA4_BLE_NOTIFY_PERIPHERAL_READY_PASS")
    link.reset_input_buffer()
    return link


def upload_paired_central(args: argparse.Namespace) -> None:
    if not CENTRAL_IMAGE.is_file():
        raise RuntimeError(f"central image does not exist: {CENTRAL_IMAGE}")
    run_command(
        [
            sys.executable,
            str(DIRECT_BOSSAC_RUNNER),
            "--bridge-port",
            args.bridge_port,
            "--relay-port",
            args.relay_port,
            "--image",
            str(CENTRAL_IMAGE),
            "--skip-runtime-smoke",
        ]
    )
    print("RA4_BLE_NOTIFY_CENTRAL_UPLOAD_PASS")


def run_central(args: argparse.Namespace) -> None:
    command = [
        sys.executable,
        str(DIRECT_RESET_RUNNER),
        "--bridge-port",
        args.bridge_port,
        "--relay-port",
        args.relay_port,
        "--capture-ms",
        str(args.central_capture_ms),
        "--attempts",
        "3",
        "--stop-when-expectations-met",
    ]
    for marker in CENTRAL_MARKERS:
        command.extend(("--expect", marker))
    run_command(command)
    print("RA4_ARDUINOBLE_NOTIFY_CENTRAL_PASS")


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
    print("PAIRED_RA4_SCAN_RESTORE_PASS")


def restore_baseline_scan(args: argparse.Namespace) -> None:
    esp32.restore_scan_smoke(
        argparse.Namespace(
            arduino_cli=args.arduino_cli,
            fqbn=FQBN,
            port=args.esp32_port,
            scan_startup_timeout_ms=100,
            scan_capture_timeout_ms=45000,
        )
    )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--relay-port", default="auto")
    parser.add_argument("--esp32-port", default="auto")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--peripheral-settle-ms", type=int, default=1500)
    parser.add_argument("--peripheral-ready-timeout-ms", type=int, default=15000)
    parser.add_argument("--central-capture-ms", type=int, default=30000)
    parser.add_argument("--peripheral-capture-ms", type=int, default=10000)
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    args.relay_port = resolve_relay_port(args.relay_port)
    args.esp32_port = resolve_baseline_esp32_port(args.esp32_port)
    if (
        args.peripheral_settle_ms < 0
        or args.peripheral_ready_timeout_ms <= 0
        or args.central_capture_ms <= 0
        or args.peripheral_capture_ms <= 0
    ):
        raise RuntimeError("invalid timeout")

    esp32.require_port_identity(args.esp32_port)
    print(f"BL616CL paired bridge: {args.bridge_port} (2341:1002 interface 00)")
    print(f"Relay control:         {args.relay_port} (0403:6010 interface 00)")
    print(f"ESP32-S3 baseline:     {args.esp32_port} (2341:1002 interface 01)")
    print("Target: both RA4M1 application Flash images are replaced temporarily.")
    print("BL616CL and ESP32-S3 firmware Flash are untouched.")
    print("Privacy: no BLE address, local name, or payload is printed or stored.")
    print("Cleanup: both RA4M1 boards will be restored to WiFiS3 scan even on failure.")

    peripheral_link: serial.Serial | None = None
    primary_error: BaseException | None = None
    try:
        compile_sketch(args.arduino_cli, PERIPHERAL_SKETCH, PERIPHERAL_BUILD_DIR)
        compile_sketch(args.arduino_cli, CENTRAL_SKETCH, CENTRAL_BUILD_DIR)
        peripheral_link = upload_baseline_peripheral(args)
        upload_paired_central(args)
        run_central(args)

        peripheral_runtime = capture_until_markers(
            peripheral_link,
            PERIPHERAL_MARKERS,
            args.peripheral_capture_ms / 1000.0,
        )
        for marker in PERIPHERAL_MARKERS:
            if marker not in peripheral_runtime:
                raise RuntimeError(f"peripheral marker was not observed: {marker}")
        print("RA4_BLE_NOTIFY_PERIPHERAL_EVENT_PASS")
        print("RA4_ARDUINOBLE_NOTIFY_GATE_PASS")
    except BaseException as error:
        primary_error = error
    finally:
        if peripheral_link is not None and peripheral_link.is_open:
            peripheral_link.close()
        try:
            print("PAIRED_RA4_SCAN_RESTORE_STARTED")
            restore_paired_scan(args)
        except BaseException as error:
            print("PAIRED_RA4_SCAN_RESTORE_FAILED", file=sys.stderr)
            if primary_error is None:
                primary_error = error
        try:
            print("ESP32_RA4_SCAN_RESTORE_STARTED")
            restore_baseline_scan(args)
        except BaseException as error:
            print("ESP32_RA4_SCAN_RESTORE_FAILED", file=sys.stderr)
            if primary_error is None:
                primary_error = error

    if primary_error is not None:
        raise primary_error
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
