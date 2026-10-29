#!/usr/bin/env python3
"""Validate final-carrier BL616CL SoftAP traffic with the Windows WLAN client.

The runner temporarily writes a RA4M1 WiFiS3 TCP-server Sketch, asks it to
create a generated WPA2 SoftAP, and attaches the Windows WLAN adapter through
a generated per-user profile. The Windows helper holds two concurrent TCP
sessions so the RA4M1 validates both WiFiServer::available() broadcast
semantics and WiFiServer::accept() semantics over the BL616CL AP data path.

The generated credentials exist only in process memory and the temporary
Windows profile. The helper removes that profile and requests restoration of
the previous WLAN profile. The RA4M1 scan Sketch is restored on every exit.
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
DEFAULT_SKETCH = ROOT_DIR / "examples" / "ra4-wifis3-tcp-server-smoke"
DEFAULT_BUILD_DIR = ROOT_DIR / "build" / "ra4-wifis3-tcp-server-final-carrier"
DEFAULT_IMAGE = DEFAULT_BUILD_DIR / "ra4-wifis3-tcp-server-smoke.ino.bin"
DEFAULT_UPLOADER = ROOT_DIR / "tools" / "run_ra4_bossac_final_carrier.py"
DEFAULT_SCAN_RESTORE = ROOT_DIR / "tools" / "run_ra4_wifis3_scan_final_carrier.py"
WINDOWS_HELPER = ROOT_DIR / "tools" / "run_bl616cl_softap_wlan_client_windows.ps1"
WSL_DISTRIBUTION = "Ubuntu-22.04"

READY_MARKER = "RA4_TCP_SERVER:READY"
PASS_MARKER = "RA4_TCP_SERVER:RESULT=PASS"
FAIL_PREFIX = "RA4_TCP_SERVER:RESULT=FAIL:"
WINDOWS_MARKERS = (
    "WINDOWS_SOFTAP_CLIENT_LOADER_STARTED",
    "WINDOWS_SOFTAP_CLIENT_LOADER_SOURCE_READY",
    "WINDOWS_SOFTAP_CLIENT_LOADER_INVOKING",
    "WINDOWS_SOFTAP_CLIENT_HELPER_STARTED",
    "WINDOWS_SOFTAP_CLIENT_ASSOCIATED",
    "WINDOWS_SOFTAP_CLIENT_BROADCAST_PASS",
    "WINDOWS_SOFTAP_CLIENT_ACCEPT_PASS",
    "WINDOWS_SOFTAP_CLIENT_TCP_CONCURRENT_SESSIONS_PASS",
    "WINDOWS_SOFTAP_CLIENT_PROFILE_REMOVED",
)


def run_command(command: list[str], *, env: dict[str, str] | None = None) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False, env=env).returncode != 0:
        raise RuntimeError("subprocess failed")


def capture_until(
    link: serial.Serial,
    required: tuple[str, ...],
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
            raise RuntimeError("RA4M1 marker capture exceeded its byte limit")
        text = received.decode("ascii", errors="replace").replace("\x00", "")
        if FAIL_PREFIX in text:
            start = text.find(FAIL_PREFIX)
            end = text.find("\n", start)
            raise RuntimeError(text[start:end if end >= 0 else len(text)].strip())
        if all(marker in text for marker in required):
            return text
    raise RuntimeError("required RA4M1 SoftAP traffic markers were not observed")


def print_ra4_markers(capture: str) -> None:
    for line in capture.splitlines():
        if line.startswith("RA4_TCP_SERVER:"):
            print(line)


def compile_server(args: argparse.Namespace) -> Path:
    sketch = args.sketch.resolve()
    image = args.image.resolve()
    if not sketch.is_dir():
        raise RuntimeError(f"RA4M1 TCP server Sketch does not exist: {sketch}")
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
        raise RuntimeError(f"compiled RA4M1 TCP server image does not exist: {image}")
    return image


def upload_server(args: argparse.Namespace, image: Path) -> None:
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
) -> None:
    # WSL does not reliably forward arbitrary process environment variables to
    # a Windows interop child. Pass short-lived Base64 text inside the
    # unlogged PowerShell command and decode it only in that child process.
    # The values are never written to a repository file or runner output.
    environment = os.environ.copy()
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
            "Write-Output 'WINDOWS_SOFTAP_CLIENT_LOADER_STARTED'; "
            "$env:BL616CL_SOFTAP_WLAN_INTERFACE = "
            "[System.Text.Encoding]::ASCII.GetString("
            f"[System.Convert]::FromBase64String('{encoded_interface}')); "
            "$env:BL616CL_SOFTAP_SSID = "
            "[System.Text.Encoding]::ASCII.GetString("
            f"[System.Convert]::FromBase64String('{encoded_ssid}')); "
            "$env:BL616CL_SOFTAP_PASSWORD = "
            "[System.Text.Encoding]::ASCII.GetString("
            f"[System.Convert]::FromBase64String('{encoded_password}')); "
            f"$source = (wsl.exe -d {WSL_DISTRIBUTION} -- cat "
            f"{WINDOWS_HELPER}) -join [Environment]::NewLine; "
            "if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; "
            "Write-Output 'WINDOWS_SOFTAP_CLIENT_LOADER_SOURCE_READY'; "
            "$helper = [scriptblock]::Create($source); "
            "Write-Output 'WINDOWS_SOFTAP_CLIENT_LOADER_INVOKING'; "
            "& $helper "
            "-Interface $env:BL616CL_SOFTAP_WLAN_INTERFACE "
            "-Ssid $env:BL616CL_SOFTAP_SSID "
            "-Password $env:BL616CL_SOFTAP_PASSWORD "
            "-Address 192.168.42.1 -Port 4243 "
            f"-ConnectTimeoutMs {args.wlan_connect_timeout_ms} "
            f"-IoTimeoutMs {args.tcp_timeout_ms}; "
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
        env=environment,
        timeout=(args.wlan_connect_timeout_ms + 2 * args.tcp_timeout_ms) / 1000.0 + 20.0,
    )
    output = result.stdout + result.stderr
    for line in output.splitlines():
        if line.startswith("WINDOWS_SOFTAP_CLIENT_"):
            print(line)
    if result.returncode != 0:
        for line in output.splitlines():
            if line.startswith("WINDOWS_SOFTAP_CLIENT_FAILURE_STAGE="):
                raise RuntimeError(
                    "Windows WLAN client traffic helper failed at " +
                    line.partition("=")[2]
                )
        raise RuntimeError(
            "Windows WLAN client traffic helper failed before helper stage "
            f"reporting (PowerShell exit {result.returncode})"
        )
    missing = [marker for marker in WINDOWS_MARKERS if marker not in output]
    if missing:
        raise RuntimeError("Windows WLAN client did not report all traffic markers")


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
    parser.add_argument("--tcp-timeout-ms", type=int, default=10000)
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
        or args.tcp_timeout_ms <= 0
    ):
        raise RuntimeError("invalid timeout")

    print(f"Bridge: {args.bridge_port} (final-carrier BL616CL 2341:1002)")
    print(f"Windows WLAN interface: {args.wlan_interface}")
    print("Target: RA4M1 application Flash only; BL616CL Flash is untouched.")
    print("Privacy: generated SoftAP credentials stay in child-process memory and a removed temporary Windows profile.")
    print("Scope: one physical Windows WLAN station with two concurrent TCP sessions; this is not a multi-station association gate.")

    server_uploaded = False
    primary_error: Exception | None = None
    bridge: serial.Serial | None = None
    try:
        image = compile_server(args)
        upload_server(args, image)
        server_uploaded = True
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
            bridge.write(f"START {ssid} {password}\n".encode("ascii"))
            bridge.flush()
            ready = capture_until(
                bridge,
                (READY_MARKER,),
                args.server_ready_timeout_ms / 1000.0,
            )
            print_ra4_markers(ready)
            print("BL616CL_SOFTAP_TCP_SERVER_READY")

            run_windows_client(args, ssid, password)
            result = capture_until(
                bridge,
                (PASS_MARKER,),
                args.traffic_timeout_ms / 1000.0,
            )
            print_ra4_markers(result)
            print("FINAL_CARRIER_SOFTAP_WLAN_TCP_TRAFFIC_PASS")
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as error:
        primary_error = error
    finally:
        if bridge is not None and bridge.is_open:
            bridge.close()
        if server_uploaded:
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
