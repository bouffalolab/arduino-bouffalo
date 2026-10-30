#!/usr/bin/env python3
"""Validate BL616CL WiFiS3 ping against the private STA network gateway.

Credentials are read only from the user-private mode-0600 file and remain in
this process. The paired RA4M1 discovers its DHCP gateway at runtime, pings
it, verifies unknown-host mapping, and is restored to scan smoke afterwards.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ra4_runtime_uart_route import open_runtime_route
from run_bl616cl_apsta_gate import (
    DEFAULT_CREDENTIAL_ENV,
    load_private_credentials,
)
import run_ra4_wifis3_scan_smoke as paired_scan
from uno_r4_usb_identity import (
    resolve_paired_bl616cl_port,
    resolve_relay_port,
)


ROOT_DIR = Path(__file__).resolve().parents[1]
FQBN = "arduino:renesas_uno:unor4wifi"
FLASH_RUNNER = ROOT_DIR / "tools" / "run_bl616cl_paired_flash.py"
BOSSAC_RUNNER = ROOT_DIR / "tools" / "run_ra4_bossac_direct.py"
PING_SKETCH = ROOT_DIR / "examples" / "ra4-wifis3-sta-ping-smoke"
PING_BUILD = ROOT_DIR / "build" / "ra4-wifis3-sta-ping-smoke"

PING_PASS = "RA4_STA_PING:RESULT=PASS"
PING_FAIL = "RA4_STA_PING:RESULT=FAIL:"


class MarkerCaptureError(RuntimeError):
    def __init__(self, marker: str, captured: str) -> None:
        super().__init__(f"RA4M1 STA ping smoke reported failure: {marker}")
        self.captured = captured


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def encode_start_value(value: str) -> str:
    return value.encode("ascii", "strict").hex()


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
    raise RuntimeError("required RA4M1 STA ping markers were not observed")


def print_markers(label: str, text: str) -> None:
    markers = [
        line.strip()
        for line in text.replace("\x00", "").splitlines()
        if line.strip().startswith("RA4_STA_PING:")
    ]
    if markers:
        print(f"{label}_BEGIN")
        for marker in markers:
            print(marker)
        print(f"{label}_END")


def compile_ping_sketch(args: argparse.Namespace) -> Path:
    run_command(
        [
            args.arduino_cli,
            "compile",
            "--fqbn",
            FQBN,
            "--build-path",
            str(PING_BUILD),
            str(PING_SKETCH),
        ]
    )
    image = PING_BUILD / "ra4-wifis3-sta-ping-smoke.ino.bin"
    if not image.is_file():
        raise RuntimeError("compiled STA ping image does not exist")
    return image


def upload_ping_sketch(args: argparse.Namespace, image: Path) -> None:
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


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--relay-port", default="auto")
    parser.add_argument("--credential-env", type=Path, default=DEFAULT_CREDENTIAL_ENV)
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--ping-timeout-ms", type=int, default=45000)
    parser.add_argument("--paired-runtime-settle-ms", type=int, default=2000)
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    args.relay_port = resolve_relay_port(args.relay_port)
    if args.ping_timeout_ms <= 0 or args.paired_runtime_settle_ms < 0:
        raise RuntimeError("invalid timeout")

    ssid, password = load_private_credentials(args.credential_env)
    if password is None:
        raise RuntimeError("STA ping gate requires a private WPA password")
    paired_scan.require_port_identity(args.bridge_port, "2341", "1002", "00")
    paired_scan.require_port_identity(args.relay_port, "0403", "6010", "00")
    print(f"BL616CL bridge: {args.bridge_port} (2341:1002 interface 00)")
    print(f"Relay:           {args.relay_port} (0403:6010 interface 00)")
    print("Target: BL616CL bridge Flash, then paired RA4M1 application Flash.")
    print("Privacy: private STA credentials, AT input, gateway address, and raw replies stay in memory.")

    route = None
    paired_modified = False
    primary_error: Exception | None = None
    try:
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
        image = compile_ping_sketch(args)
        upload_ping_sketch(args, image)
        paired_modified = True

        route = open_runtime_route(
            args.bridge_port,
            args.relay_port,
            bridge_timeout=1.0,
            receiver_timeout=0.1,
        )
        route.bridge.reset_input_buffer()
        route.bridge.write(
            (
                "START "
                + encode_start_value(ssid)
                + " "
                + encode_start_value(password)
                + "\n"
            ).encode("ascii")
        )
        route.bridge.flush()
        capture = capture_until(
            route.receiver,
            (
                "RA4_STA_PING:WIFI_CONNECTED",
                "RA4_STA_PING:GATEWAY_VALID",
                "RA4_STA_PING:GATEWAY_PASS",
                "RA4_STA_PING:UNKNOWN_HOST_PASS",
                PING_PASS,
            ),
            PING_FAIL,
            args.ping_timeout_ms / 1000.0,
        )
        print_markers("PAIRED_STA_PING", capture)
        print("RA4_WIFIS3_STA_PING_GATE_PASS")
    except MarkerCaptureError as error:
        print_markers("RA4_STA_PING_DIAGNOSTICS", error.captured)
        primary_error = error
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        primary_error = error
    finally:
        if route is not None:
            route.close()
        if paired_modified:
            try:
                restore_paired_scan(args)
            except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError):
                print("PAIRED_RA4_SCAN_RESTORE_FAILED", file=sys.stderr)
                if primary_error is None:
                    primary_error = RuntimeError("paired RA4M1 scan restore failed")

    if primary_error is not None:
        raise primary_error
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
