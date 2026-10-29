#!/usr/bin/env python3
"""Validate BL616CL DNS-compatible hostname connect and TCP echo via WiFiS3.

The original ESP32-S3 UNO R4 temporarily runs an isolated SoftAP and TCP echo
server. Its generated credentials are sent only over the two RA4M1 user UART
paths, retained in process memory, and never printed or written to a file.
Both RA4M1 boards are restored to the standard WiFiS3 scan smoke on cleanup.
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
import run_ra4_at_uart_gate as paired
import run_ra4_wifis3_scan_smoke as paired_scan
import run_esp32_sta_baseline_gate as esp32
from ra4_runtime_uart_route import open_runtime_route
from uno_r4_usb_identity import (
    resolve_baseline_esp32_port,
    resolve_paired_bl616cl_port,
    resolve_relay_port,
)


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_FLASH_RUNNER = ROOT_DIR / "tools" / "run_bl616cl_paired_flash.py"
DEFAULT_BOSSAC_RUNNER = ROOT_DIR / "tools" / "run_ra4_bossac_direct.py"
ESP32_ECHO_SKETCH = ROOT_DIR / "examples" / "ra4-wifis3-softap-tcp-echo"
ESP32_ECHO_BUILD = ROOT_DIR / "build" / "esp32-ra4-wifis3-softap-tcp-echo"
PAIRED_CLIENT_SKETCH = ROOT_DIR / "examples" / "ra4-wifis3-tcp-client-smoke"
PAIRED_CLIENT_BUILD = ROOT_DIR / "build" / "ra4-wifis3-tcp-client-smoke"
FQBN = "arduino:renesas_uno:unor4wifi"

ESP32_BOOT = "RA4_TCP_ECHO:BOOT"
ESP32_READY = "RA4_TCP_ECHO:READY"
PAIRED_BOOT = "RA4_TCP_CLIENT:BOOT"
PAIRED_WAIT = "RA4_TCP_CLIENT:WAIT_START"
PAIRED_SEND_PASS = "RA4_TCP_CLIENT:TCP_SEND_PASS"
PAIRED_RECEIVE_PASS = "RA4_TCP_CLIENT:TCP_RECEIVE_PASS"
PAIRED_PASS = "RA4_TCP_CLIENT:RESULT=PASS"
PAIRED_FAIL = "RA4_TCP_CLIENT:RESULT=FAIL:"
ESP32_EXCHANGE = "RA4_TCP_ECHO:EXCHANGE"
ESP32_EXCHANGE_EMPTY = "RA4_TCP_ECHO:EXCHANGE_EMPTY"


class MarkerCaptureError(RuntimeError):
    def __init__(self, marker: str, captured: str) -> None:
        super().__init__(f"RA4M1 smoke reported failure: {marker}")
        self.captured = captured


def print_safe_markers(label: str, text: str, prefix: str) -> None:
    markers = [
        line.strip()
        for line in text.replace("\x00", "").splitlines()
        if line.strip().startswith(prefix)
    ]
    if not markers:
        return
    print(f"{label}_BEGIN")
    for marker in markers:
        print(marker)
    print(f"{label}_END")


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def open_serial(port: str, timeout: float, dtr: bool = False) -> serial.Serial:
    link = serial.Serial(port, 115200, timeout=timeout, write_timeout=timeout)
    link.dtr = dtr
    link.rts = False
    return link


def capture_until(
    link: serial.Serial,
    required: tuple[str, ...],
    fail_prefix: str | None,
    timeout_seconds: float,
    maximum_bytes: int = 8192,
    any_required: tuple[str, ...] = (),
) -> str:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        payload = link.read(512)
        if payload:
            received.extend(payload)
            if len(received) > maximum_bytes:
                raise RuntimeError("serial marker capture exceeded its byte limit")
            text = received.decode("ascii", errors="replace").replace("\x00", "")
            if fail_prefix is not None and fail_prefix in text:
                marker_start = text.find(fail_prefix)
                marker_end = text.find("\n", marker_start)
                if marker_end < 0:
                    marker_end = len(text)
                marker = text[marker_start:marker_end].strip()
                raise MarkerCaptureError(marker, text)
            if all(marker in text for marker in required) and (
                not any_required or any(marker in text for marker in any_required)
            ):
                return text
    raise RuntimeError("required RA4M1 smoke markers were not observed")


def compile_esp32_echo(args: argparse.Namespace) -> None:
    run_command(
        [
            args.arduino_cli,
            "compile",
            "--fqbn",
            FQBN,
            "--build-path",
            str(ESP32_ECHO_BUILD),
            str(ESP32_ECHO_SKETCH),
        ]
    )


def upload_esp32_echo(args: argparse.Namespace) -> None:
    compile_esp32_echo(args)
    run_command(
        [
            args.arduino_cli,
            "upload",
            "--fqbn",
            FQBN,
            "--port",
            args.esp32_port,
            "--input-dir",
            str(ESP32_ECHO_BUILD),
        ]
    )


def compile_paired_client(args: argparse.Namespace) -> Path:
    run_command(
        [
            args.arduino_cli,
            "compile",
            "--fqbn",
            FQBN,
            "--build-path",
            str(PAIRED_CLIENT_BUILD),
            str(PAIRED_CLIENT_SKETCH),
        ]
    )
    image = PAIRED_CLIENT_BUILD / "ra4-wifis3-tcp-client-smoke.ino.bin"
    if not image.is_file():
        raise RuntimeError("compiled paired RA4M1 client image does not exist")
    return image


def flash_paired_bridge(args: argparse.Namespace) -> None:
    run_command(
        [
            sys.executable,
            str(DEFAULT_FLASH_RUNNER),
            "--bridge-port",
            args.bridge_port,
            "--relay-port",
            args.relay_port,
        ]
    )


def upload_paired_client(args: argparse.Namespace, image: Path) -> None:
    run_command(
        [
            sys.executable,
            str(DEFAULT_BOSSAC_RUNNER),
            "--bridge-port",
            args.bridge_port,
            "--relay-port",
            args.relay_port,
            "--image",
            str(image),
            "--runtime-settle-ms",
            str(args.paired_runtime_settle_ms),
            "--skip-runtime-smoke",
        ]
    )


def restore_esp32_scan(args: argparse.Namespace) -> None:
    esp32.restore_scan_smoke(
        argparse.Namespace(
            arduino_cli=args.arduino_cli,
            fqbn=FQBN,
            port=args.esp32_port,
            scan_startup_timeout_ms=100,
            scan_capture_timeout_ms=45000,
        )
    )
    print("ESP32_RA4_SCAN_RESTORE_PASS")


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
    parser.add_argument("--server-ready-timeout-ms", type=int, default=30000)
    parser.add_argument("--client-timeout-ms", type=int, default=60000)
    parser.add_argument("--paired-runtime-settle-ms", type=int, default=2000)
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    args.relay_port = resolve_relay_port(args.relay_port)
    args.esp32_port = resolve_baseline_esp32_port(args.esp32_port)
    if (
        args.server_ready_timeout_ms <= 0
        or args.client_timeout_ms <= 0
        or args.paired_runtime_settle_ms < 0
    ):
        raise RuntimeError("invalid timeout")

    paired.require_port_identity(args.bridge_port, "2341", "1002", "00")
    paired.require_port_identity(args.relay_port, "0403", "6010", "00")
    if args.legacy_reference_port is not None:
        paired.require_port_identity(args.legacy_reference_port, "0403", "6010", "00")
    esp32.require_port_identity(args.esp32_port)
    print(f"BL616CL bridge: {args.bridge_port} (2341:1002 interface 00)")
    print(f"Relay:           {args.relay_port} (0403:6010 interface 00)")
    print(f"ESP32-S3 peer:   {args.esp32_port} (2341:1002 original baseline)")
    print("Target: BL616CL Flash, then both RA4M1 application Flash areas.")
    print("Privacy: generated SoftAP credentials and test payload stay in memory.")

    ap_ssid = "uno-r4-" + secrets.token_hex(4)
    ap_password = secrets.token_urlsafe(18)
    server: serial.Serial | None = None
    bridge: serial.Serial | None = None
    reference: serial.Serial | None = None
    route = None
    esp32_modified = False
    paired_modified = False
    primary_error: Exception | None = None

    try:
        upload_esp32_echo(args)
        esp32_modified = True
        # The original ESP32-S3 bridge forwards its USB CDC stream only with
        # DTR asserted. Do not rely on the one-shot BOOT marker because it
        # can be emitted before Arduino CLI returns from upload.
        server = open_serial(args.esp32_port, 0.1, dtr=True)
        server.reset_input_buffer()
        server.write(f"START {ap_ssid} {ap_password}\n".encode("ascii"))
        server.flush()
        capture_until(
            server,
            (ESP32_READY,),
            "RA4_TCP_ECHO:RESULT=FAIL:",
            args.server_ready_timeout_ms / 1000.0,
        )
        print("ESP32_TCP_ECHO_SERVER_READY")

        flash_paired_bridge(args)
        image = compile_paired_client(args)
        upload_paired_client(args, image)
        paired_modified = True

        route = open_runtime_route(
            args.bridge_port,
            args.relay_port,
            args.legacy_reference_port,
            bridge_timeout=1.0,
            receiver_timeout=0.1,
        )
        bridge = route.bridge
        reference = route.receiver
        bridge.reset_input_buffer()
        reference.reset_input_buffer()
        bridge.write(f"START {ap_ssid} {ap_password} 192.168.4.1 4242\n".encode("ascii"))
        bridge.flush()
        try:
            client_capture = capture_until(
                reference,
                (
                    "RA4_TCP_CLIENT:WIFI_CONNECTED",
                    "RA4_TCP_CLIENT:HOST_BY_NAME_PASS",
                    "RA4_TCP_CLIENT:TCP_CONNECT_PASS",
                    PAIRED_SEND_PASS,
                    PAIRED_RECEIVE_PASS,
                    "RA4_TCP_CLIENT:TCP_ECHO_PASS",
                    PAIRED_PASS,
                ),
                PAIRED_FAIL,
                args.client_timeout_ms / 1000.0,
            )
        except MarkerCaptureError as error:
            print_safe_markers(
                "RA4_TCP_CLIENT_DIAGNOSTICS",
                error.captured,
                "RA4_TCP_CLIENT:",
            )
            try:
                server_capture = capture_until(
                    server,
                    (),
                    "RA4_TCP_ECHO:RESULT=FAIL:",
                    10.0,
                    any_required=(ESP32_EXCHANGE, ESP32_EXCHANGE_EMPTY),
                )
                print_safe_markers(
                    "RA4_TCP_ECHO_DIAGNOSTICS",
                    server_capture,
                    "RA4_TCP_ECHO:",
                )
            except MarkerCaptureError as server_error:
                print_safe_markers(
                    "RA4_TCP_ECHO_DIAGNOSTICS",
                    server_error.captured,
                    "RA4_TCP_ECHO:",
                )
            except RuntimeError:
                print("RA4_TCP_ECHO_DIAGNOSTICS_UNAVAILABLE")
            raise
        print_safe_markers(
            "RA4_TCP_CLIENT_DIAGNOSTICS",
            client_capture,
            "RA4_TCP_CLIENT:",
        )
        server_capture = capture_until(
            server,
            (),
            "RA4_TCP_ECHO:RESULT=FAIL:",
            10.0,
            any_required=(ESP32_EXCHANGE, ESP32_EXCHANGE_EMPTY),
        )
        print_safe_markers(
            "RA4_TCP_ECHO_DIAGNOSTICS",
            server_capture,
            "RA4_TCP_ECHO:",
        )
        if ESP32_EXCHANGE_EMPTY in server_capture:
            raise RuntimeError("TCP echo server accepted a client but received no payload")
        print("RA4_WIFIS3_TCP_ECHO_GATE_PASS")
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        primary_error = error
    finally:
        if route is not None:
            route.close()
        if server is not None and server.is_open:
            server.close()
        if paired_modified:
            try:
                restore_paired_scan(args)
            except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError):
                print("PAIRED_RA4_SCAN_RESTORE_FAILED", file=sys.stderr)
                if primary_error is None:
                    primary_error = RuntimeError("paired RA4M1 scan restore failed")
        if esp32_modified:
            try:
                restore_esp32_scan(args)
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
