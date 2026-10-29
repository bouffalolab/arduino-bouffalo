#!/usr/bin/env python3
"""Validate final-carrier BL616CL AP+STA TCP/UDP coexistence with Windows WLAN.

The RA4M1 Sketch configures the BL616CL bridge in AP+STA mode through its AT
UART, joins the user-private upstream STA, starts a generated local SoftAP,
and keeps the upstream association alive while Windows performs TCP and UDP
traffic over that SoftAP. Credentials and IP addresses stay out of output.
"""

from __future__ import annotations

import argparse
import base64
import os
import re
import secrets
import subprocess
import sys
import time
from pathlib import Path

import serial

from run_bl616cl_apsta_gate import load_private_credentials
from read_bl616cl_windows_telemetry import (
    discover_windows_bus_id,
    discover_wsl_usbip_bus_id,
    reattach_bridge,
    reattach_bridge_via_wsl_usbip,
    resolve_wsl_usbip_host,
)
from run_bl616cl_final_carrier_flash import resolve_ftdi_port
from run_ra4_wifis3_scan_final_carrier import require_final_carrier_bridge
from uno_r4_usb_identity import resolve_paired_bl616cl_port


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_FQBN = "arduino:renesas_uno:unor4wifi"
DEFAULT_SKETCH = ROOT_DIR / "examples" / "ra4-wifis3-apsta-traffic"
DEFAULT_BUILD_DIR = ROOT_DIR / "build" / "ra4-wifis3-apsta-final-carrier"
DEFAULT_IMAGE = DEFAULT_BUILD_DIR / "ra4-wifis3-apsta-traffic.ino.bin"
DEFAULT_UPLOADER = ROOT_DIR / "tools" / "run_ra4_bossac_final_carrier.py"
DEFAULT_SCAN_RESTORE = ROOT_DIR / "tools" / "run_ra4_wifis3_scan_final_carrier.py"
DEFAULT_USB_RECOVERY = ROOT_DIR / "tools" / "recover_bl616cl_final_carrier_usb.py"
WINDOWS_HELPER = ROOT_DIR / "tools" / "run_bl616cl_apsta_wlan_client_windows.ps1"
WINDOWS_TELEMETRY = ROOT_DIR / "tools" / "read_bl616cl_windows_telemetry.py"
LOCAL_TELEMETRY = ROOT_DIR / "tools" / "inspect_bl616cl_gpio35.py"
WSL_DISTRIBUTION = "Ubuntu-22.04"
WINDOWS_TELEMETRY_ATTEMPTS = 1
WINDOWS_TELEMETRY_TIMEOUT_SECONDS = 60.0

READY_MARKERS = (
    "RA4_APSTA:STA_CONNECTED",
    "RA4_APSTA:UPSTREAM_PING_BEFORE_AP_PASS",
    "RA4_APSTA:SOFTAP_READY",
    "RA4_APSTA:UPSTREAM_PING_AFTER_AP_PASS",
    "RA4_APSTA:READY",
)
PASS_MARKER = "RA4_APSTA:RESULT=PASS"
FAIL_PREFIX = "RA4_APSTA:RESULT=FAIL:"
TRAFFIC_START_MARKER = "RA4_APSTA:TRAFFIC_START"
WINDOWS_MARKERS = (
    "WINDOWS_APSTA_CLIENT_HELPER_STARTED",
    "WINDOWS_APSTA_CLIENT_SOFTAP_VISIBLE",
    "WINDOWS_APSTA_CLIENT_ASSOCIATED",
    "WINDOWS_APSTA_CLIENT_TCP_BROADCAST_PASS",
    "WINDOWS_APSTA_CLIENT_TCP_ACCEPT_PASS",
    "WINDOWS_APSTA_CLIENT_TCP_CONCURRENT_SESSIONS_PASS",
    "WINDOWS_APSTA_CLIENT_UDP_PASS",
    "WINDOWS_APSTA_CLIENT_TRAFFIC_PASS",
    "WINDOWS_APSTA_CLIENT_PROFILE_REMOVED",
)


class Ra4ApstaCaptureError(RuntimeError):
    """A terminal AP+STA capture failure that retains safe RA4 progress text."""

    def __init__(self, message: str, capture: str) -> None:
        super().__init__(message)
        self.capture = capture


class Ra4ApstaUsbDisconnectError(Ra4ApstaCaptureError):
    """The bridge USB CDC disappeared while RA4M1 waited for AP+STA."""


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def capture_until(
    link: serial.Serial,
    required: tuple[str, ...],
    timeout_seconds: float,
) -> str:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        try:
            payload = link.read(512)
        except serial.SerialException as error:
            raise Ra4ApstaUsbDisconnectError(
                "BL616CL CDC disappeared while waiting for AP+STA",
                received.decode("ascii", errors="replace").replace("\x00", ""),
            ) from error
        if not payload:
            continue
        received.extend(payload)
        if len(received) > 16384:
            raise RuntimeError("RA4M1 AP+STA marker capture exceeded its byte limit")
        text = received.decode("ascii", errors="replace").replace("\x00", "")
        if FAIL_PREFIX in text:
            start = text.find(FAIL_PREFIX)
            end = text.find("\n", start)
            raise Ra4ApstaCaptureError(
                text[start:end if end >= 0 else len(text)].strip(),
                text,
            )
        if all(marker in text for marker in required):
            return text
    raise Ra4ApstaCaptureError(
        "required RA4M1 AP+STA markers were not observed",
        received.decode("ascii", errors="replace").replace("\x00", ""),
    )


def print_ra4_markers(capture: str) -> None:
    for line in capture.splitlines():
        if line.startswith("RA4_APSTA:") or line.startswith((
            "REQUEST:",
            "   ANSWER:",
            "   Result:",
            "RAW:",
            "<-RAW END",
        )):
            print(line)


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


def open_flash_boot_control(ftdi_port: str) -> serial.Serial:
    """Keep BOOT at Flash-start level across an internal BL616CL reset."""
    control = serial.Serial(
        port=None,
        baudrate=2000000,
        timeout=0.1,
        write_timeout=0.1,
    )
    # Final carrier wiring is DTR# -> BOOT and RTS# -> RESET. Configure before
    # opening so an AP-start WDT reset cannot sample the USB BootROM strap.
    control.dtr = True
    control.rts = False
    control.port = ftdi_port
    control.open()
    return control


def hex_field(value: str) -> str:
    return value.encode("ascii", "strict").hex()


def run_windows_client(
    args: argparse.Namespace,
    ssid: str,
    password: str,
    bridge: serial.Serial | None = None,
) -> str:
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
            "Write-Output 'WINDOWS_APSTA_CLIENT_LOADER_STARTED'; "
            "$env:BL616CL_APSTA_WLAN_INTERFACE = "
            "[System.Text.Encoding]::ASCII.GetString("
            f"[System.Convert]::FromBase64String('{encoded_interface}')); "
            "$env:BL616CL_APSTA_SSID = "
            "[System.Text.Encoding]::ASCII.GetString("
            f"[System.Convert]::FromBase64String('{encoded_ssid}')); "
            "$env:BL616CL_APSTA_PASSWORD = "
            "[System.Text.Encoding]::ASCII.GetString("
            f"[System.Convert]::FromBase64String('{encoded_password}')); "
            f"$source = (wsl.exe -d {WSL_DISTRIBUTION} -- cat "
            f"{WINDOWS_HELPER}) -join [Environment]::NewLine; "
            "if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; "
            "$helper = [scriptblock]::Create($source); "
            "& $helper "
            "-Interface $env:BL616CL_APSTA_WLAN_INTERFACE "
            "-Ssid $env:BL616CL_APSTA_SSID "
            "-Password $env:BL616CL_APSTA_PASSWORD "
            f"-ConnectTimeoutMs {args.wlan_connect_timeout_ms} "
            f"-IoTimeoutMs {args.io_timeout_ms}; "
            "exit $LASTEXITCODE"
        ),
    ]
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=os.environ.copy(),
    )
    ra4_capture = bytearray()
    deadline = time.monotonic() + (
        args.wlan_connect_timeout_ms + 3 * args.io_timeout_ms
    ) / 1000.0 + 20.0
    while process.poll() is None:
        if time.monotonic() >= deadline:
            process.kill()
            raise RuntimeError("Windows AP+STA traffic helper timed out")
        if bridge is not None:
            payload = bridge.read(512)
            if payload:
                ra4_capture.extend(payload)
                if len(ra4_capture) > 16384:
                    del ra4_capture[:-8192]
        else:
            time.sleep(0.05)
    stdout, stderr = process.communicate(timeout=5.0)
    output = stdout + stderr
    ra4_text = ra4_capture.decode("ascii", errors="replace").replace("\x00", "")
    print_ra4_markers(ra4_text)
    for line in output.splitlines():
        if line.startswith("WINDOWS_APSTA_CLIENT_"):
            print(line)
    if process.returncode != 0:
        for line in output.splitlines():
            if line.startswith("WINDOWS_APSTA_CLIENT_FAILURE_STAGE="):
                raise RuntimeError(
                    "Windows AP+STA traffic helper failed at " +
                    line.partition("=")[2]
                )
        raise RuntimeError(
            "Windows AP+STA traffic helper failed before helper stage "
            f"reporting (PowerShell exit {process.returncode})"
        )
    missing = [marker for marker in WINDOWS_MARKERS if marker not in output]
    if missing:
        raise RuntimeError("Windows AP+STA traffic helper did not report all markers")
    return ra4_text


def trigger_traffic(link: serial.Serial) -> None:
    """Start the fixture timeout only when the Windows client is ready to run."""
    link.write(b"TRAFFIC\n")
    link.flush()
    capture = capture_until(link, (TRAFFIC_START_MARKER,), 5.0)
    print_ra4_markers(capture)
    print("BL616CL_APSTA_TRAFFIC_START_PASS")


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


def bridge_is_available(port: str) -> bool:
    try:
        require_final_carrier_bridge(port)
        return True
    except RuntimeError:
        return False


def bridge_cdc_is_openable(port: str) -> bool:
    """Require the re-enumerated CDC node to accept a real serial open."""
    try:
        probe = serial.Serial(
            port=None,
            baudrate=115200,
            timeout=0.05,
            write_timeout=0.05,
        )
        probe.dtr = False
        probe.rts = False
        probe.port = port
        probe.open()
        probe.close()
        return True
    except (OSError, serial.SerialException):
        return False


def wait_for_internal_usb_recovery(
    args: argparse.Namespace,
    timeout_seconds: float,
    require_disconnect: bool = False,
) -> str:
    """Reattach the final carrier after its own USB runtime reset.

    This intentionally does not pulse FTDI RESET: the retained page-8 record
    must survive the internal recovery so the AP+STA failure boundary remains
    observable.
    """
    deadline = time.monotonic() + timeout_seconds
    host = resolve_wsl_usbip_host("auto")
    last_attach_attempt = 0.0
    observed_disconnect = False
    while time.monotonic() < deadline:
        try:
            port = resolve_paired_bl616cl_port("auto")
            require_final_carrier_bridge(port)
            if (
                (not require_disconnect or observed_disconnect)
                and bridge_cdc_is_openable(port)
            ):
                # CDC can be present a little before the HID interface is
                # usable after USB/IP reattach. Give the composite device a
                # short settle interval before the page-8 feature exchange.
                time.sleep(0.75)
                if bridge_cdc_is_openable(port):
                    return port
        except RuntimeError:
            observed_disconnect = True
            now = time.monotonic()
            if now - last_attach_attempt >= 0.5:
                last_attach_attempt = now
                try:
                    bus_id = discover_windows_bus_id()
                    reattach_bridge(
                        WSL_DISTRIBUTION,
                        host,
                        bus_id,
                        "/dev/ttyACM0",
                        1.0,
                    )
                except RuntimeError:
                    try:
                        bus_id = discover_wsl_usbip_bus_id(host)
                        reattach_bridge_via_wsl_usbip(host, bus_id)
                    except RuntimeError:
                        pass
        time.sleep(0.1)
    raise RuntimeError(
        "BL616CL CDC/HID did not recover after the AP+STA USB disconnect"
    )


def recover_bridge(args: argparse.Namespace) -> None:
    run_command(
        [
            sys.executable,
            str(DEFAULT_USB_RECOVERY),
            "--bridge-port",
            args.bridge_port,
        ]
    )


def capture_live_ap_telemetry(
    args: argparse.Namespace,
    require_ready: bool = True,
) -> None:
    """Sample current FHOST VIF state before Windows scans for the SoftAP."""
    if not require_ready:
        # During the AP+STA failure path, retain USB ownership in WSL and use
        # hidraw directly. This avoids turning a bridge-runtime failure into
        # an unrelated WSL-to-Windows interop failure before page-8 can record
        # the liveness/FHOST boundary.
        for option in (
            "--ap-start",
            "--ap-start-watchdog",
            "--ap-start-create-ap",
            "--ap-start-hostapd-lifecycle",
            "--ap-start-status-code",
            "--ap-start-hostapd-state-update",
        ):
            command = [
                sys.executable,
                str(LOCAL_TELEMETRY),
                option,
            ]
            print("COMMAND=" + " ".join(command))
            result = subprocess.run(
                command,
                check=False,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=5.0,
            )
            output = result.stdout + result.stderr
            for line in output.splitlines():
                if line.startswith((
                    "HIDRAW=",
                    "AP_START=",
                    "AP_START_CHANNEL=",
                    "AP_START_TIMING=",
                    "AP_START_WATCHDOG=",
                    "AP_START_RETAINED=",
                    "AP_START_CREATE_AP=",
                    "AP_START_HOSTAPD_LIFECYCLE=",
                    "AP_START_HOSTAPD_LIFECYCLE_RECORD=",
                    "AP_START_STATUS_CODE=",
                    "AP_START_HOSTAPD_STATE_UPDATE=",
                    "AP_START_HOSTAPD_STATE_UPDATE_RECORD=",
                    "AP_START_TELEMETRY_PASS",
                    "AP_START_WATCHDOG_TELEMETRY_PASS",
                    "AP_START_CREATE_AP_TELEMETRY_PASS",
                    "AP_START_HOSTAPD_LIFECYCLE_TELEMETRY_PASS",
                    "AP_START_STATUS_CODE_TELEMETRY_PASS",
                    "AP_START_HOSTAPD_STATE_UPDATE_TELEMETRY_PASS",
                )):
                    print(line)
            if result.returncode != 0:
                raise RuntimeError(
                    f"local AP-start HID telemetry capture failed: {option}"
                )
        print("BL616CL_APSTA_FAILURE_AP_TELEMETRY_CAPTURED")
        return

    outputs: dict[str, str] = {}
    for page in (
        "ap-start",
        "ap-start-wpa-event-path",
        "ap-start-create-ap",
        "ap-start-hostapd-lifecycle",
        "ap-start-hostapd-state-update",
        "ap-start-status-code",
    ):
        command = [
            sys.executable,
            str(WINDOWS_TELEMETRY),
            page,
            "--bridge-port",
            args.bridge_port,
            "--telemetry-attempts",
            str(WINDOWS_TELEMETRY_ATTEMPTS),
        ]
        print("COMMAND=" + " ".join(command))
        result = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=WINDOWS_TELEMETRY_TIMEOUT_SECONDS,
        )
        output = result.stdout + result.stderr
        outputs[page] = output
        for line in output.splitlines():
            if line.startswith((
                "CMSIS-DAP HID path:",
                "AP start:",
                "AP start channel:",
                "AP start WPA event:",
                "AP start create AP:",
                "AP start hostapd lifecycle:",
                "AP start hostapd lifecycle[",
                "AP start hostapd state updates:",
                "AP start hostapd state update[",
                "AP start status code:",
            )):
                print(line)
        if result.returncode != 0:
            raise RuntimeError(f"Windows live AP telemetry capture failed: {page}")

    output = outputs["ap-start"]
    state = next(
        (line for line in output.splitlines() if line.startswith("AP start:")),
        None,
    )
    if state is None:
        raise RuntimeError("Windows live AP telemetry did not return AP state")
    match = re.fullmatch(
        r"AP start:.*\bsta_connected=(\d+)\s+ap_started=(\d+)",
        state,
    )
    if match is None:
        raise RuntimeError("Windows live AP telemetry returned an invalid AP state")
    if match.group(1) != "1" or match.group(2) != "1":
        raise RuntimeError("AP+STA ready marker disagrees with live FHOST VIF state")
    print("BL616CL_APSTA_LIVE_AP_STATE_PASS")


def capture_tcp_failure_telemetry(args: argparse.Namespace) -> None:
    """Capture the live listener state before restoring the RA4M1 Sketch."""
    for page in (
        "udp-socket",
        "ap-udp-packet-input",
        "tcp-runtime",
        "tcp-data-plane",
        "ap-packet-input",
        "tcp-listener-lwip",
        "tcp-listener-close",
    ):
        command = [
            sys.executable,
            str(WINDOWS_TELEMETRY),
            page,
            "--bridge-port",
            args.bridge_port,
            "--telemetry-attempts",
            str(WINDOWS_TELEMETRY_ATTEMPTS),
        ]
        print("COMMAND=" + " ".join(command))
        result = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=WINDOWS_TELEMETRY_TIMEOUT_SECONDS,
        )
        output = result.stdout + result.stderr
        for line in output.splitlines():
            if line.startswith((
                "CMSIS-DAP HID path:",
                "TCP listener:",
                "TCP accept:",
                "TCP accepted socket:",
                "TCP available probes:",
                "AP input:",
                "AP input last:",
                "AP listener output:",
                "AP UDP input:",
                "AP UDP input last:",
                "AP UDP output last:",
                "UDP socket:",
                "UDP socket stats:",
                "UDP backend receive:",
                "UDP parse:",
                "TCP listener lwIP:",
                "TCP listener lwIP pcb[",
                "TCP listener lwIP socket:",
                "TCP listener close:",
                "TCP_RUNTIME_TELEMETRY_PASS",
                "TCP_DATA_PLANE_TELEMETRY_PASS",
                "AP_PACKET_INPUT_TELEMETRY_PASS",
                "TCP_LISTENER_LWIP_TELEMETRY_PASS",
                "BL616CL_WINDOWS_TELEMETRY_PASS",
            )):
                print(line)
        if result.returncode != 0:
            print(f"BL616CL_APSTA_TCP_TELEMETRY_FAILED page={page}")
            continue
        print(f"BL616CL_APSTA_TCP_TELEMETRY_PASS page={page}")


def capture_tcp_listener_preflight_telemetry(args: argparse.Namespace) -> None:
    """Verify the AP+STA listener exists before a Windows client connects."""
    for page in ("tcp-runtime", "tcp-listener-lwip", "tcp-listener-close"):
        command = [
            sys.executable,
            str(WINDOWS_TELEMETRY),
            page,
            "--bridge-port",
            args.bridge_port,
            "--telemetry-attempts",
            str(WINDOWS_TELEMETRY_ATTEMPTS),
        ]
        print("COMMAND=" + " ".join(command))
        result = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=WINDOWS_TELEMETRY_TIMEOUT_SECONDS,
        )
        output = result.stdout + result.stderr
        for line in output.splitlines():
            if line.startswith((
                "CMSIS-DAP HID path:",
                "TCP listener:",
                "TCP accept:",
                "TCP listener lwIP:",
                "TCP listener lwIP pcb[",
                "TCP listener lwIP socket:",
                "TCP listener close:",
                "TCP_RUNTIME_TELEMETRY_PASS",
                "TCP_LISTENER_LWIP_TELEMETRY_PASS",
                "TCP_LISTENER_CLOSE_TELEMETRY_PASS",
            )):
                print(line)
        if result.returncode != 0:
            raise RuntimeError(
                "Windows TCP listener preflight telemetry failed: " + page
            )
        print(f"BL616CL_APSTA_TCP_PREFLIGHT_PASS page={page}")


def capture_ra4_failure_markers(args: argparse.Namespace) -> None:
    """Drain concise RA4M1 progress markers before Windows takes the HID device."""
    deadline = time.monotonic() + 1.0
    received = bytearray()
    with serial.Serial(
        args.bridge_port,
        baudrate=115200,
        timeout=0.1,
        write_timeout=1.0,
    ) as bridge:
        bridge.dtr = False
        bridge.rts = False
        while time.monotonic() < deadline:
            payload = bridge.read(512)
            if payload:
                received.extend(payload)
            if len(received) > 4096:
                break
    for line in received.decode("ascii", errors="replace").splitlines():
        if line.startswith("RA4_APSTA:"):
            print(line)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--ftdi-port", default="auto")
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
    parser.add_argument("--cdc-recovery-timeout-ms", type=int, default=20000)
    parser.add_argument(
        "--skip-live-ap-telemetry",
        action="store_true",
        help="skip the Windows HID AP-state sample after RA4M1 reports READY",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    args.ftdi_port = resolve_ftdi_port(args.ftdi_port)
    require_final_carrier_bridge(args.bridge_port)
    if (
        args.runtime_settle_ms < 0
        or args.ready_timeout_ms <= 0
        or args.traffic_timeout_ms <= 0
        or args.wlan_connect_timeout_ms <= 0
        or args.io_timeout_ms <= 0
        or args.cdc_recovery_timeout_ms <= 0
    ):
        raise RuntimeError("invalid timeout")

    upstream_ssid, upstream_password = load_private_credentials(
        Path.home() / ".config" / "uno-r4-wifi-bridge" / "sta-test.env"
    )
    if upstream_password is None:
        raise RuntimeError("AP+STA gate requires a private WPA password")

    print(f"Bridge: {args.bridge_port} (final-carrier BL616CL 2341:1002)")
    print("Target: RA4M1 application Flash only; BL616CL Flash is untouched.")
    print("Privacy: upstream and generated SoftAP credentials, addresses, and raw AT frames stay in memory.")
    print("Scope: STA upstream persistence plus one Windows SoftAP client running concurrent TCP and UDP traffic.")

    sketch_uploaded = False
    apsta_ready = False
    internal_usb_recovered = False
    primary_error: Exception | None = None
    boot_control: serial.Serial | None = None
    try:
        boot_control = open_flash_boot_control(args.ftdi_port)
        print("BL616CL_APSTA_BOOT_SOURCE=FLASH")
        # Opening the FTDI can briefly re-enumerate the native composite USB.
        # Re-resolve it after BOOT is held at the Flash-start level instead of
        # handing a stale CDC pathname to the RA4M1 uploader.
        args.bridge_port = wait_for_internal_usb_recovery(
            args,
            args.cdc_recovery_timeout_ms / 1000.0,
        )
        print(f"BL616CL_APSTA_CDC_READY={args.bridge_port}")
        image = compile_sketch(args)
        upload_sketch(args, image)
        sketch_uploaded = True
        time.sleep(args.runtime_settle_ms / 1000.0)

        local_ssid = "uno-r4-" + secrets.token_hex(4)
        local_password = secrets.token_urlsafe(18)
        start = (
            "START " +
            " ".join(
                (
                    hex_field(upstream_ssid),
                    hex_field(upstream_password),
                    hex_field(local_ssid),
                    hex_field(local_password),
                )
            ) +
            "\n"
        ).encode("ascii")

        with serial.Serial(
            args.bridge_port,
            baudrate=115200,
            timeout=0.1,
            write_timeout=1.0,
        ) as bridge:
            bridge.dtr = False
            bridge.rts = False
            bridge.reset_input_buffer()
            bridge.write(start)
            bridge.flush()
            ready = capture_until(
                bridge,
                READY_MARKERS,
                args.ready_timeout_ms / 1000.0,
            )
            print_ra4_markers(ready)
            print("BL616CL_APSTA_FINAL_CARRIER_READY")
            apsta_ready = True

            if args.skip_live_ap_telemetry:
                trigger_traffic(bridge)
                traffic = run_windows_client(
                    args,
                    local_ssid,
                    local_password,
                    bridge,
                )
                if PASS_MARKER not in traffic:
                    raise RuntimeError(
                        "Windows AP+STA traffic completed without the "
                        "RA4M1 success marker"
                    )
                print_ra4_markers(traffic)
                print("BL616CL_APSTA_FINAL_CARRIER_TRAFFIC_PASS")

        if not args.skip_live_ap_telemetry:
            capture_live_ap_telemetry(args)
            capture_tcp_listener_preflight_telemetry(args)
            with serial.Serial(
                args.bridge_port,
                baudrate=115200,
                timeout=0.1,
                write_timeout=1.0,
            ) as bridge:
                bridge.dtr = False
                bridge.rts = False
                trigger_traffic(bridge)
                traffic = run_windows_client(
                    args,
                    local_ssid,
                    local_password,
                    bridge,
                )
                if PASS_MARKER in traffic:
                    print_ra4_markers(traffic)
                    print("BL616CL_APSTA_FINAL_CARRIER_TRAFFIC_PASS")
                else:
                    result = capture_until(
                        bridge,
                        (PASS_MARKER,),
                        args.traffic_timeout_ms / 1000.0,
                    )
                    print_ra4_markers(result)
                    print("BL616CL_APSTA_FINAL_CARRIER_TRAFFIC_PASS")
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as error:
        primary_error = error
        if isinstance(error, Ra4ApstaUsbDisconnectError):
            print("RA4_APSTA_USB_DISCONNECT_BEGIN")
            print_ra4_markers(error.capture)
            print("RA4_APSTA_USB_DISCONNECT_END")
            try:
                args.bridge_port = wait_for_internal_usb_recovery(
                    args,
                    args.cdc_recovery_timeout_ms / 1000.0,
                    require_disconnect=True,
                )
                internal_usb_recovered = True
                print(
                    "BL616CL_APSTA_INTERNAL_USB_RECOVERY_PASS "
                    f"port={args.bridge_port}"
                )
                capture_live_ap_telemetry(args, require_ready=False)
            except (
                OSError,
                RuntimeError,
                subprocess.SubprocessError,
            ) as telemetry_error:
                print(
                    "BL616CL_APSTA_FAILURE_AP_TELEMETRY_FAILED="
                    + str(telemetry_error)
                )
        elif isinstance(error, Ra4ApstaCaptureError):
            print("RA4_APSTA_CAPTURE_FAILURE_BEGIN")
            print_ra4_markers(error.capture)
            print("RA4_APSTA_CAPTURE_FAILURE_END")
            if "RA4_APSTA:STAGE=SOFTAP_WAIT" in error.capture:
                try:
                    capture_live_ap_telemetry(args, require_ready=False)
                except (
                    OSError,
                    RuntimeError,
                    subprocess.SubprocessError,
                ):
                    print("BL616CL_APSTA_FAILURE_AP_TELEMETRY_FAILED")
    finally:
        if primary_error is not None and apsta_ready:
            try:
                capture_ra4_failure_markers(args)
            except (OSError, serial.SerialException):
                print("RA4_APSTA_FAILURE_MARKER_CAPTURE_FAILED")
            try:
                capture_tcp_failure_telemetry(args)
            except (OSError, RuntimeError, subprocess.SubprocessError):
                print("BL616CL_APSTA_TCP_TELEMETRY_FAILED")
        if sketch_uploaded:
            try:
                if not internal_usb_recovered and not bridge_is_available(args.bridge_port):
                    if boot_control is not None and boot_control.is_open:
                        boot_control.close()
                    recover_bridge(args)
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
        if boot_control is not None and boot_control.is_open:
            boot_control.close()

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
