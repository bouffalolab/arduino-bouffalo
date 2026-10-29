#!/usr/bin/env python3
"""Run the final-carrier AP+STA gate while Windows owns the BL616CL USB device.

WSL builds and uploads the temporary RA4M1 test Sketch.  The script then gives
the composite CDC/HID device to Windows for the entire AP+STA interval so
SoftAP startup, HID telemetry, Windows WLAN association, and TCP/UDP traffic
do not cross a USB/IP ownership boundary.  It finally reattaches WSL and
restores the normal RA4M1 WiFiS3 scan smoke Sketch.
"""

from __future__ import annotations

import argparse
import base64
import secrets
import subprocess
import sys
import time
from pathlib import Path

from run_bl616cl_apsta_gate import load_private_credentials
from run_bl616cl_apsta_wlan_final_carrier import (
    DEFAULT_BUILD_DIR,
    DEFAULT_FQBN,
    DEFAULT_IMAGE,
    DEFAULT_SCAN_RESTORE,
    DEFAULT_SKETCH,
    DEFAULT_UPLOADER,
    require_final_carrier_bridge,
)
from read_bl616cl_windows_telemetry import (
    discover_windows_bus_id,
    reattach_bridge,
    resolve_wsl_usbip_host,
)
from uno_r4_usb_identity import resolve_paired_bl616cl_port


ROOT_DIR = Path(__file__).resolve().parents[1]
WINDOWS_HELPER = ROOT_DIR / "tools" / "run_bl616cl_apsta_windows_owned.ps1"
USB_RECOVERY = ROOT_DIR / "tools" / "recover_bl616cl_final_carrier_usb.py"
WINDOWS_WSL_ROOT = r"\\wsl.localhost\Ubuntu-22.04"
WSL_DISTRIBUTION = "Ubuntu-22.04"


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def compile_sketch(args: argparse.Namespace) -> Path:
    sketch = args.sketch.resolve()
    image = args.image.resolve()
    if not sketch.is_dir():
        raise RuntimeError(f"RA4M1 AP+STA Sketch does not exist: {sketch}")
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
        raise RuntimeError(f"compiled RA4M1 AP+STA image does not exist: {image}")
    return image


def upload_sketch(args: argparse.Namespace, image: Path) -> None:
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


def detach_bridge_to_windows(bus_id: str, timeout_seconds: float) -> str:
    script = (
        "$ErrorActionPreference = 'Stop'; "
        f"usbipd wsl detach --busid {bus_id}; "
        "$deadline = (Get-Date).AddSeconds("
        f"{timeout_seconds:.3f}"
        "); "
        "$ports = @(); "
        "do { "
        "$ports = @(Get-CimInstance Win32_SerialPort | Where-Object { "
        "$_.PNPDeviceID -match 'VID_2341&PID_1002&MI_00\\\\' "
        "}); "
        "if ($ports.Count -eq 1) { break }; "
        "Start-Sleep -Milliseconds 250 "
        "} while ((Get-Date) -lt $deadline); "
        "if ($ports.Count -ne 1) { "
        "throw \"expected one healthy final-carrier Windows CDC port, found $($ports.Count)\" "
        "}; "
        "Write-Output $ports[0].DeviceID"
    )
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-Command", script],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout_seconds + 15.0,
    )
    if result.returncode != 0 or not result.stdout.strip():
        detail = (result.stderr or result.stdout).strip()
        raise RuntimeError(
            "Windows did not regain the final-carrier BL616CL CDC interface" +
            (f": {detail}" if detail else "")
        )
    port = result.stdout.strip().splitlines()[-1].strip()
    if not port.startswith("COM"):
        raise RuntimeError(f"Windows reported an invalid BL616CL CDC port: {port}")
    return port


def run_windows_gate(
    args: argparse.Namespace,
    windows_port: str,
    upstream_ssid: str,
    upstream_password: str,
    local_ssid: str,
    local_password: str,
) -> None:
    windows_helper = WINDOWS_WSL_ROOT + str(WINDOWS_HELPER)
    upstream_ssid_base64 = base64.b64encode(
        upstream_ssid.encode("ascii")
    ).decode("ascii")
    upstream_password_base64 = base64.b64encode(
        upstream_password.encode("ascii")
    ).decode("ascii")
    softap_ssid_base64 = base64.b64encode(
        local_ssid.encode("ascii")
    ).decode("ascii")
    softap_password_base64 = base64.b64encode(
        local_password.encode("ascii")
    ).decode("ascii")
    command = [
        "powershell.exe",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-Command",
        (
            "$ErrorActionPreference = 'Stop'; "
            f"$env:BL616CL_APSTA_UPSTREAM_SSID_B64 = '{upstream_ssid_base64}'; "
            f"$env:BL616CL_APSTA_UPSTREAM_PASSWORD_B64 = '{upstream_password_base64}'; "
            f"$env:BL616CL_APSTA_SOFTAP_SSID_B64 = '{softap_ssid_base64}'; "
            f"$env:BL616CL_APSTA_SOFTAP_PASSWORD_B64 = '{softap_password_base64}'; "
            f"& '{windows_helper}' "
            f"-BridgePort {windows_port} "
            "-UpstreamSsidBase64 $env:BL616CL_APSTA_UPSTREAM_SSID_B64 "
            "-UpstreamPasswordBase64 $env:BL616CL_APSTA_UPSTREAM_PASSWORD_B64 "
            "-SoftApSsidBase64 $env:BL616CL_APSTA_SOFTAP_SSID_B64 "
            "-SoftApPasswordBase64 $env:BL616CL_APSTA_SOFTAP_PASSWORD_B64 "
            f"-WlanInterface '{args.wlan_interface}' "
            f"-ReadyTimeoutMs {args.ready_timeout_ms} "
            f"-TrafficTimeoutMs {args.traffic_timeout_ms} "
            f"-WlanConnectTimeoutMs {args.wlan_connect_timeout_ms} "
            f"-IoTimeoutMs {args.io_timeout_ms} "
            f"-CdcRecoveryTimeoutMs {args.cdc_recovery_timeout_ms}; "
            "exit $LASTEXITCODE"
        ),
    ]
    print("COMMAND=powershell.exe <Windows-owned AP+STA gate; credentials redacted>")
    result = subprocess.run(
        command,
        check=False,
        text=True,
        timeout=(
            args.ready_timeout_ms + args.traffic_timeout_ms +
            args.wlan_connect_timeout_ms + 3 * args.io_timeout_ms
        ) / 1000.0 + 60.0,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"Windows-owned AP+STA gate failed with exit status {result.returncode}"
        )


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


def recover_bridge(args: argparse.Namespace) -> None:
    run_command(
        [
            sys.executable,
            str(USB_RECOVERY),
            "--bridge-port",
            args.bridge_port,
        ]
    )
    print("WINDOWS_APSTA_USB_RECOVERY_PASS")


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
    parser.add_argument("--ready-timeout-ms", type=int, default=45000)
    parser.add_argument("--traffic-timeout-ms", type=int, default=60000)
    parser.add_argument("--wlan-connect-timeout-ms", type=int, default=30000)
    parser.add_argument("--io-timeout-ms", type=int, default=10000)
    parser.add_argument("--cdc-recovery-timeout-ms", type=int, default=10000)
    parser.add_argument("--windows-ready-timeout-seconds", type=int, default=15)
    parser.add_argument("--reattach-timeout-ms", type=int, default=15000)
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier_bridge(args.bridge_port)
    if (
        args.runtime_settle_ms < 0
        or args.ready_timeout_ms <= 0
        or args.traffic_timeout_ms <= 0
        or args.wlan_connect_timeout_ms <= 0
        or args.io_timeout_ms <= 0
        or args.cdc_recovery_timeout_ms <= 0
        or args.windows_ready_timeout_seconds <= 0
        or args.reattach_timeout_ms <= 0
    ):
        raise RuntimeError("invalid AP+STA timeout")
    if not WINDOWS_HELPER.is_file():
        raise RuntimeError(f"Windows-owned AP+STA helper is missing: {WINDOWS_HELPER}")

    upstream_ssid, upstream_password = load_private_credentials(
        Path.home() / ".config" / "uno-r4-wifi-bridge" / "sta-test.env"
    )
    if upstream_password is None:
        raise RuntimeError("AP+STA gate requires a private WPA password")

    print(f"Bridge: {args.bridge_port} (final-carrier BL616CL 2341:1002)")
    print("Target: RA4M1 application Flash only; BL616CL Flash is untouched.")
    print("Ownership: WSL uploads; Windows owns CDC/HID for the entire AP+STA gate.")
    print("Privacy: all STA and generated SoftAP credentials stay in process memory.")

    sketch_uploaded = False
    bus_id = ""
    wsl_attached = True
    primary_error: Exception | None = None
    try:
        image = compile_sketch(args)
        upload_sketch(args, image)
        sketch_uploaded = True
        time.sleep(args.runtime_settle_ms / 1000.0)

        bus_id = discover_windows_bus_id()
        windows_port = detach_bridge_to_windows(
            bus_id,
            args.windows_ready_timeout_seconds,
        )
        wsl_attached = False
        print(f"WINDOWS_APSTA_OWNER_PORT={windows_port}")
        run_windows_gate(
            args,
            windows_port,
            upstream_ssid,
            upstream_password,
            "uno-r4-" + secrets.token_hex(4),
            secrets.token_urlsafe(18),
        )
        print("BL616CL_APSTA_WINDOWS_OWNED_GATE_PASS")
    except (
        OSError,
        RuntimeError,
        subprocess.SubprocessError,
        subprocess.TimeoutExpired,
    ) as error:
        primary_error = error
    finally:
        if bus_id:
            try:
                reattach_bridge(
                    WSL_DISTRIBUTION,
                    resolve_wsl_usbip_host("auto"),
                    bus_id,
                    args.bridge_port,
                    args.reattach_timeout_ms / 1000.0,
                )
                wsl_attached = True
                print("WINDOWS_APSTA_WSL_REATTACH_PASS")
            except RuntimeError:
                print("WINDOWS_APSTA_WSL_REATTACH_FAILED", file=sys.stderr)
                try:
                    recover_bridge(args)
                    wsl_attached = True
                except (
                    OSError,
                    RuntimeError,
                    subprocess.SubprocessError,
                ) as recovery_error:
                    if primary_error is None:
                        primary_error = recovery_error
                    else:
                        print(
                            "WINDOWS_APSTA_USB_RECOVERY_FAILED",
                            file=sys.stderr,
                        )
        if sketch_uploaded:
            try:
                if not wsl_attached:
                    recover_bridge(args)
                    wsl_attached = True
                require_final_carrier_bridge(args.bridge_port)
                restore_scan(args)
            except (
                OSError,
                RuntimeError,
                subprocess.SubprocessError,
            ) as error:
                if primary_error is None:
                    primary_error = error
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
        subprocess.SubprocessError,
        subprocess.TimeoutExpired,
    ) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
