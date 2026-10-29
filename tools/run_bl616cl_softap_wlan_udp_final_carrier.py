#!/usr/bin/env python3
"""Validate final-carrier BL616CL SoftAP UDP traffic with the Windows WLAN client.

The runner temporarily writes the RA4M1 WiFiS3 SoftAP UDP echo Sketch,
connects the Windows WLAN interface with a generated WPA2 profile, and checks
one real UDP request/reply exchange, or a requested number of distinct
16 KiB datagrams with peer and content verification and board-observed ACKs.
Generated credentials remain only in
process memory and the temporary Windows WLAN profile. The RA4M1 scan Sketch
is restored on every exit.
"""

from __future__ import annotations

import argparse
import base64
import os
import secrets
import subprocess
import sys
import time
from pathlib import Path

import serial

from run_ra4_wifis3_scan_final_carrier import require_final_carrier_bridge
from uno_r4_usb_identity import resolve_paired_bl616cl_port


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_FQBN = "arduino:renesas_uno:unor4wifi"
DEFAULT_SKETCH = ROOT_DIR / "examples" / "ra4-wifis3-softap-udp-echo"
DEFAULT_BUILD_DIR = ROOT_DIR / "build" / "ra4-wifis3-softap-udp-final-carrier"
DEFAULT_IMAGE = DEFAULT_BUILD_DIR / "ra4-wifis3-softap-udp-echo.ino.bin"
DEFAULT_UPLOADER = ROOT_DIR / "tools" / "run_ra4_bossac_final_carrier.py"
DEFAULT_SCAN_RESTORE = ROOT_DIR / "tools" / "run_ra4_wifis3_scan_final_carrier.py"
WINDOWS_HELPER = ROOT_DIR / "tools" / "run_bl616cl_softap_wlan_udp_client_windows.ps1"
WSL_DISTRIBUTION = "Ubuntu-22.04"

READY_MARKER = "RA4_UDP_ECHO:READY"
PASS_MARKER = "RA4_UDP_ECHO:RESULT=PASS"
FAIL_PREFIX = "RA4_UDP_ECHO:RESULT=FAIL:"
ADDRESS_PREFIX = "RA4_UDP_ECHO:ADDRESS="
WINDOWS_MARKERS = (
    "WINDOWS_SOFTAP_UDP_CLIENT_LOADER_STARTED",
    "WINDOWS_SOFTAP_UDP_CLIENT_LOADER_SOURCE_READY",
    "WINDOWS_SOFTAP_UDP_CLIENT_LOADER_INVOKING",
    "WINDOWS_SOFTAP_UDP_CLIENT_HELPER_STARTED",
    "WINDOWS_SOFTAP_UDP_CLIENT_ASSOCIATED",
    "WINDOWS_SOFTAP_UDP_CLIENT_SEND_PASS",
    "WINDOWS_SOFTAP_UDP_CLIENT_REPLY_PASS",
    "WINDOWS_SOFTAP_UDP_CLIENT_DATAGRAM_PASS",
    "WINDOWS_SOFTAP_UDP_CLIENT_PROFILE_REMOVED",
)


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def capture_until(
    link: serial.Serial,
    required: tuple[str, ...],
    timeout_seconds: float,
    byte_limit: int = 8192,
) -> str:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        payload = link.read(512)
        if not payload:
            continue
        received.extend(payload)
        if len(received) > byte_limit:
            raise RuntimeError("RA4M1 marker capture exceeded its byte limit")
        text = received.decode("ascii", errors="replace").replace("\x00", "")
        if FAIL_PREFIX in text:
            print_ra4_markers(text)
            start = text.find(FAIL_PREFIX)
            end = text.find("\n", start)
            raise RuntimeError(text[start:end if end >= 0 else len(text)].strip())
        if all(marker in text for marker in required):
            return text
    raise RuntimeError("required RA4M1 SoftAP UDP markers were not observed")


def print_ra4_markers(capture: str) -> None:
    for line in capture.splitlines():
        if line.startswith("RA4_UDP_ECHO:"):
            print(line)


def extract_address(capture: str) -> str:
    for line in capture.splitlines():
        if line.startswith(ADDRESS_PREFIX):
            address = line[len(ADDRESS_PREFIX):].strip()
            octets = address.split(".")
            if len(octets) == 4 and all(
                octet.isdecimal() and 0 <= int(octet) <= 255
                for octet in octets
            ):
                return address
    raise RuntimeError("RA4M1 SoftAP UDP Sketch did not report a valid IPv4 address")


def compile_echo(args: argparse.Namespace) -> Path:
    sketch = args.sketch.resolve()
    image = args.image.resolve()
    if not sketch.is_dir():
        raise RuntimeError(f"RA4M1 UDP echo Sketch does not exist: {sketch}")
    run_command(
        [
            args.arduino_cli,
            "compile",
            "--fqbn",
            args.fqbn,
            "--build-path",
            str(args.build_dir.resolve()),
            str(sketch),
        ]
    )
    if not image.is_file():
        raise RuntimeError(f"compiled RA4M1 UDP echo image does not exist: {image}")
    return image


def upload_echo(args: argparse.Namespace, image: Path) -> None:
    run_command(
        [
            sys.executable,
            str(DEFAULT_UPLOADER),
            "--bridge-port",
            args.bridge_port,
            "--image",
            str(image),
            "--runtime-settle-ms",
            str(args.runtime_settle_ms),
        ]
    )


def run_windows_client(
    args: argparse.Namespace,
    ssid: str,
    password: str,
    address: str,
) -> None:
    encoded_ssid = base64.b64encode(ssid.encode("ascii")).decode("ascii")
    encoded_password = base64.b64encode(password.encode("ascii")).decode("ascii")
    encoded_interface = base64.b64encode(
        args.wlan_interface.encode("ascii")
    ).decode("ascii")
    command = [
        "powershell.exe",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-Command",
        (
            "$ErrorActionPreference = 'Stop'; "
            "Write-Output 'WINDOWS_SOFTAP_UDP_CLIENT_LOADER_STARTED'; "
            "$env:BL616CL_SOFTAP_WLAN_INTERFACE = "
            "[System.Text.Encoding]::ASCII.GetString("
            f"[System.Convert]::FromBase64String('{encoded_interface}')); "
            "$env:BL616CL_SOFTAP_SSID = "
            "[System.Text.Encoding]::ASCII.GetString("
            f"[System.Convert]::FromBase64String('{encoded_ssid}')); "
            "$env:BL616CL_SOFTAP_PASSWORD = "
            "[System.Text.Encoding]::ASCII.GetString("
            f"[System.Convert]::FromBase64String('{encoded_password}')); "
            "$source = [System.IO.File]::ReadAllText("
            f"'\\\\wsl.localhost\\{WSL_DISTRIBUTION}{str(WINDOWS_HELPER).replace('/', chr(92))}'); "
            "Write-Output 'WINDOWS_SOFTAP_UDP_CLIENT_LOADER_SOURCE_READY'; "
            "$helper = [scriptblock]::Create($source); "
            "Write-Output 'WINDOWS_SOFTAP_UDP_CLIENT_LOADER_INVOKING'; "
            "& $helper "
            "-Interface $env:BL616CL_SOFTAP_WLAN_INTERFACE "
            "-Ssid $env:BL616CL_SOFTAP_SSID "
            "-Password $env:BL616CL_SOFTAP_PASSWORD "
            f"-Address {address} -Port 4244 "
            f"-ConnectTimeoutMs {args.wlan_connect_timeout_ms} "
            f"-IoTimeoutMs {args.udp_timeout_ms} -FragmentRounds {args.fragment_rounds}; "
            "exit $LASTEXITCODE"
        ),
    ]
    result = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=os.environ.copy(),
        timeout=(args.wlan_connect_timeout_ms + args.udp_timeout_ms) / 1000.0 +
                20.0 + args.fragment_rounds * 15.0,
    )
    output = result.stdout + result.stderr
    for line in output.splitlines():
        if line.startswith("WINDOWS_SOFTAP_UDP_CLIENT_"):
            print(line)
    if result.returncode != 0:
        for line in output.splitlines():
            if line.startswith("WINDOWS_SOFTAP_UDP_CLIENT_FAILURE_STAGE="):
                raise RuntimeError(
                    "Windows WLAN UDP client failed at " + line.partition("=")[2]
                )
        raise RuntimeError(
            "Windows WLAN UDP client failed before helper stage reporting "
            f"(PowerShell exit {result.returncode})"
        )
    missing = [marker for marker in WINDOWS_MARKERS if marker not in output]
    if missing:
        raise RuntimeError("Windows WLAN UDP client did not report all markers")
    if args.fragment_rounds:
        observed = [line.partition("=")[2] for line in output.splitlines()
                    if line.startswith("WINDOWS_SOFTAP_UDP_CLIENT_FRAGMENT_ROUND_PASS=")]
        if observed != [str(i) for i in range(1, args.fragment_rounds + 1)]:
            raise RuntimeError("Windows fragment rounds were incomplete or duplicated")


def restore_scan(args: argparse.Namespace) -> None:
    run_command(
        [
            sys.executable,
            str(DEFAULT_SCAN_RESTORE),
            "--bridge-port",
            args.bridge_port,
            "--arduino-cli",
            args.arduino_cli,
        ]
    )
    print("RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_PASS")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--fqbn", default=DEFAULT_FQBN)
    parser.add_argument("--sketch", type=Path, default=DEFAULT_SKETCH)
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR)
    parser.add_argument("--image", type=Path, default=DEFAULT_IMAGE)
    parser.add_argument("--wlan-interface", default="WLAN")
    parser.add_argument("--runtime-settle-ms", type=int, default=1000)
    parser.add_argument("--server-ready-timeout-ms", type=int, default=30000)
    parser.add_argument("--traffic-timeout-ms", type=int, default=45000)
    parser.add_argument("--wlan-connect-timeout-ms", type=int, default=30000)
    parser.add_argument("--udp-timeout-ms", type=int, default=10000)
    parser.add_argument("--fragment-rounds", type=int, default=0,
                        help="Require this many distinct 16 KiB SoftAP-to-WLAN datagrams.")
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier_bridge(args.bridge_port)
    if (
        args.runtime_settle_ms < 0
        or args.server_ready_timeout_ms <= 0
        or args.traffic_timeout_ms <= 0
        or args.wlan_connect_timeout_ms <= 0
        or args.udp_timeout_ms <= 0
        or not 0 <= args.fragment_rounds <= 1000
    ):
        raise RuntimeError("invalid timeout")

    print(f"Bridge: {args.bridge_port} (final-carrier BL616CL 2341:1002)")
    print(f"Windows WLAN interface: {args.wlan_interface}")
    print("Target: RA4M1 application Flash only; BL616CL Flash is untouched.")
    print("Privacy: generated SoftAP credentials stay in child-process memory and a removed temporary Windows profile.")
    print(f"Scope: direct SoftAP/WLAN UDP; fragment rounds={args.fragment_rounds}.")

    echo_uploaded = False
    primary_error: Exception | None = None
    try:
        image = compile_echo(args)
        upload_echo(args, image)
        echo_uploaded = True
        time.sleep(args.runtime_settle_ms / 1000.0)

        with serial.Serial(
            args.bridge_port,
            baudrate=115200,
            timeout=0.1,
            write_timeout=1.0,
        ) as bridge:
            bridge.dtr = False
            bridge.rts = False
            bridge.reset_input_buffer()

            ssid = "uno-r4-" + secrets.token_hex(4)
            password = secrets.token_urlsafe(18)
            start = f"FRAGSTART {args.fragment_rounds}" if args.fragment_rounds else "START"
            bridge.write(f"{start} {ssid} {password}\n".encode("ascii"))
            bridge.flush()
            ready = capture_until(
                bridge,
                (READY_MARKER, "RA4_UDP_ECHO:SELFTEST_PASS", ADDRESS_PREFIX),
                args.server_ready_timeout_ms / 1000.0,
            )
            print_ra4_markers(ready)
            address = extract_address(ready)
            print("BL616CL_SOFTAP_UDP_ECHO_READY")

            try:
                run_windows_client(args, ssid, password, address)
            except RuntimeError:
                # Retain board failure diagnostics before the scan restoration.
                try:
                    capture_until(bridge, (PASS_MARKER,), 2.0)
                except RuntimeError:
                    pass
                raise
            result = capture_until(
                bridge,
                (PASS_MARKER,),
                args.traffic_timeout_ms / 1000.0,
                byte_limit=max(8192, args.fragment_rounds * 64 + 1024),
            )
            print_ra4_markers(result)
            if args.fragment_rounds:
                observed = [line.partition("=")[2].strip() for line in result.splitlines()
                            if line.startswith("RA4_UDP_ECHO:FRAGMENT_ROUND_PASS=")]
                if observed != [str(i) for i in range(1, args.fragment_rounds + 1)]:
                    raise RuntimeError("RA4M1 fragment ACK rounds were incomplete or duplicated")
            print("FINAL_CARRIER_SOFTAP_WLAN_UDP_TRAFFIC_PASS")
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as error:
        primary_error = error
    finally:
        if echo_uploaded:
            try:
                restore_scan(args)
            except (
                OSError,
                RuntimeError,
                serial.SerialException,
                subprocess.SubprocessError,
            ) as restore_error:
                if primary_error is None:
                    primary_error = restore_error
                else:
                    print("RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_FAILED", file=sys.stderr)

    if primary_error is not None:
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
