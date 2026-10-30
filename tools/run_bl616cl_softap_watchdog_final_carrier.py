#!/usr/bin/env python3
"""Exercise the BL616CL SoftAP watchdog diagnostic on the final carrier.

The diagnostic bridge image must be built with
BL616CL_AP_START_DIAGNOSTIC_WATCHDOG=1. This runner temporarily installs the
RA4M1 Serial/Serial2 AT proxy through the final carrier's single CDC path,
requests a generated SoftAP, requires the expected BL616CL watchdog USB reset,
and reads the persisted HID feature telemetry after recovery.

It intentionally does not require the retired 0403:6010 UART relay or a
second ESP32-S3 UNO R4 board. Generated SoftAP credentials stay in memory and
are never printed or written to disk.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import serial

from run_bl616cl_final_carrier_flash import resolve_ftdi_port
from read_bl616cl_windows_telemetry import (
    discover_windows_bus_id,
    discover_wsl_usbip_bus_id,
    reattach_bridge,
    reattach_bridge_via_wsl_usbip,
    resolve_wsl_usbip_host,
)
from uno_r4_usb_identity import resolve_paired_bl616cl_port, tty_properties


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_PROXY_RUNNER = ROOT_DIR / "tools" / "run_ra4_at_uart_final_carrier.py"
DEFAULT_SCAN_RUNNER = ROOT_DIR / "tools" / "run_ra4_wifis3_scan_final_carrier.py"
DEFAULT_TELEMETRY = ROOT_DIR / "tools" / "inspect_bl616cl_gpio35.py"

TERMINAL = b"OK\r\n"
AP_RESPONSE = b"+BEGINSOFTAP: 1\r\nOK\r\n"
WATCHDOG_PATTERN = re.compile(
    r"AP_START_WATCHDOG=.*enabled=1 state=[34] .*persisted=1"
)


def require_final_carrier_bridge(port: str) -> None:
    properties = tty_properties(port)
    if (
        properties.get("ID_VENDOR_ID", "").lower() != "2341"
        or properties.get("ID_MODEL_ID", "").lower() != "1002"
        or properties.get("ID_USB_INTERFACE_NUM", "").lower() != "00"
        or properties.get("ID_SERIAL_SHORT") != "BL616CL-PROVISIONAL"
    ):
        raise RuntimeError(f"{port} is not the final-carrier BL616CL CDC interface")


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def quote_at_argument(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def read_until_terminal(link: serial.Serial, timeout_seconds: float) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    response = bytearray()
    while time.monotonic() < deadline:
        if len(response) >= 512:
            raise RuntimeError("AT response exceeded 512 bytes")
        payload = link.read(min(256, 512 - len(response)))
        if payload:
            response.extend(payload)
            if response.endswith(TERMINAL):
                return bytes(response)
    raise RuntimeError("AT response timed out")


def transact_exact(
    link: serial.Serial,
    request: bytes,
    expected: bytes,
    label: str,
    timeout_seconds: float,
) -> None:
    link.reset_input_buffer()
    link.write(request)
    link.flush()
    if read_until_terminal(link, timeout_seconds) != expected:
        raise RuntimeError(f"{label} did not match expected AT framing")


def attach_bridge_to_wsl(wsl_distribution: str) -> None:
    """Attach the final-carrier USB device after a native USB reset.

    Do this through the USB/IP service itself.  `powershell.exe` can be
    temporarily unavailable from WSL even while the USB/IP export remains
    healthy and the board is ready to be reattached.
    """
    del wsl_distribution
    try:
        host = resolve_wsl_usbip_host("auto")
        try:
            bus_id = discover_windows_bus_id()
            reattach_bridge(
                WSL_DISTRIBUTION,
                host,
                bus_id,
                "/dev/ttyACM0",
                1.0,
            )
            print(f"USBIPD_FINAL_CARRIER bus={bus_id} state=windows-attached")
        except RuntimeError:
            bus_id = discover_wsl_usbip_bus_id(host)
            reattach_bridge_via_wsl_usbip(host, bus_id)
            print(f"USBIPD_FINAL_CARRIER bus={bus_id} state=direct-attached")
    except RuntimeError as error:
        print(
            "USBIPD_FINAL_CARRIER_ATTACH_FAILED=" + str(error),
            file=sys.stderr,
        )


def wait_for_watchdog_recovery(
    timeout_seconds: float,
    wsl_distribution: str,
) -> str:
    deadline = time.monotonic() + timeout_seconds
    observed_disconnect = False
    last_attach = 0.0
    while time.monotonic() < deadline:
        try:
            bridge_port = resolve_paired_bl616cl_port("auto")
            require_final_carrier_bridge(bridge_port)
            if observed_disconnect:
                return bridge_port
        except RuntimeError:
            observed_disconnect = True
            now = time.monotonic()
            if now - last_attach >= 0.5:
                last_attach = now
                attach_bridge_to_wsl(wsl_distribution)
        time.sleep(0.1)
    if not observed_disconnect:
        raise RuntimeError("SoftAP request did not cause the diagnostic watchdog USB reset")
    raise RuntimeError("BL616CL USB did not recover after the diagnostic watchdog reset")


def wait_for_bridge_ready(
    timeout_seconds: float,
    wsl_distribution: str,
) -> str:
    """Wait for normal USB after an earlier diagnostic or runner failure."""
    deadline = time.monotonic() + timeout_seconds
    last_attach = 0.0
    while time.monotonic() < deadline:
        try:
            bridge_port = resolve_paired_bl616cl_port("auto")
            require_final_carrier_bridge(bridge_port)
            return bridge_port
        except RuntimeError:
            now = time.monotonic()
            if now - last_attach >= 0.5:
                last_attach = now
                attach_bridge_to_wsl(wsl_distribution)
        time.sleep(0.1)
    raise RuntimeError("BL616CL final-carrier USB did not become available")


def ensure_hidraw_access(wsl_distribution: str) -> str:
    from inspect_bl616cl_gpio35 import find_hidraw

    hidraw = find_hidraw()
    if os.access(hidraw, os.R_OK | os.W_OK):
        return hidraw

    command = [
        "powershell.exe",
        "-NoProfile",
        "-Command",
        (
            f"wsl.exe -d {wsl_distribution} -u root -- "
            f"/bin/chmod a+rw {hidraw}; exit $LASTEXITCODE"
        ),
    ]
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError(f"could not grant temporary HID access to {hidraw}")
    if not os.access(hidraw, os.R_OK | os.W_OK):
        raise RuntimeError(f"HID node is not readable and writable: {hidraw}")
    return hidraw


def open_flash_boot_control(ftdi_port: str) -> serial.Serial:
    """Hold BOOT at the normal Flash-start level across an internal WDT reset."""
    control = serial.Serial(
        port=None,
        baudrate=2000000,
        timeout=0.1,
        write_timeout=0.1,
    )
    # The final carrier routes DTR# to BOOT and RTS# to RESET. Configure the
    # desired idle levels before opening so an AP-start watchdog reset samples
    # Flash boot rather than falling into the USB BootROM.
    control.dtr = True
    control.rts = False
    control.port = ftdi_port
    control.open()
    return control


def read_telemetry(
    option: str,
    hidraw: str,
) -> str:
    result = subprocess.run(
        [sys.executable, str(DEFAULT_TELEMETRY), "--hidraw", hidraw, option],
        check=False,
        capture_output=True,
        text=True,
        timeout=5.0,
    )
    if result.stdout:
        print(result.stdout, end="")
    if result.returncode != 0:
        detail = result.stderr.strip()
        raise RuntimeError(detail or f"could not read {option} telemetry")
    return result.stdout


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--ftdi-port", default="auto")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--response-timeout-ms", type=int, default=2000)
    parser.add_argument("--runtime-settle-ms", type=int, default=1000)
    parser.add_argument("--watchdog-recovery-timeout-ms", type=int, default=20000)
    parser.add_argument(
        "--expected-notification-ab-last-phase",
        type=int,
        default=None,
        help=(
            "require a specific ENABLE_NETWORK event-path A/B telemetry phase "
            "after watchdog recovery; omit while collecting a new boundary"
        ),
    )
    parser.add_argument("--wsl-distribution", default="Ubuntu-22.04")
    parser.add_argument(
        "--skip-proxy-upload",
        action="store_true",
        help="Reuse the temporary RA4M1 AT proxy already installed.",
    )
    parser.add_argument(
        "--keep-proxy",
        action="store_true",
        help="Leave the RA4M1 AT proxy installed after the diagnostic.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if (
        args.response_timeout_ms <= 0
        or args.runtime_settle_ms < 0
        or args.watchdog_recovery_timeout_ms <= 0
        or (
            args.expected_notification_ab_last_phase is not None
            and not 1 <= args.expected_notification_ab_last_phase <= 8
        )
    ):
        raise RuntimeError("invalid timeout")

    requested_bridge_port = args.bridge_port
    args.ftdi_port = resolve_ftdi_port(args.ftdi_port)
    response_timeout = args.response_timeout_ms / 1000.0
    proxy_uploaded = False
    primary_error: Exception | None = None
    boot_control: serial.Serial | None = None

    print(f"FTDI: {args.ftdi_port} (0403:6001 BOOT/RESET control)")
    print("Target: BL616CL SoftAP watchdog diagnostic; RA4M1 proxy is temporary.")
    print("Privacy: generated SoftAP credentials and raw AT responses stay in memory.")
    try:
        boot_control = open_flash_boot_control(args.ftdi_port)
        print("FINAL_CARRIER_SOFTAP_BOOT_SOURCE=FLASH")
        args.bridge_port = wait_for_bridge_ready(
            args.watchdog_recovery_timeout_ms / 1000.0,
            args.wsl_distribution,
        )
        if (
            requested_bridge_port != "auto"
            and args.bridge_port != requested_bridge_port
        ):
            raise RuntimeError(
                "the requested bridge port did not match the final-carrier "
                f"CDC port: requested {requested_bridge_port}, "
                f"found {args.bridge_port}"
            )
        require_final_carrier_bridge(args.bridge_port)
        print(f"Bridge: {args.bridge_port} (final-carrier BL616CL 2341:1002)")
        print(f"FINAL_CARRIER_SOFTAP_CDC_READY={args.bridge_port}")
        if not args.skip_proxy_upload:
            run_command(
                [
                    sys.executable,
                    str(DEFAULT_PROXY_RUNNER),
                    "--bridge-port",
                    args.bridge_port,
                    "--arduino-cli",
                    args.arduino_cli,
                    "--skip-status-check",
                    "--keep-proxy",
                ]
            )
            proxy_uploaded = True
            time.sleep(args.runtime_settle_ms / 1000.0)

        with serial.Serial(
            args.bridge_port,
            baudrate=115200,
            timeout=0.1,
            write_timeout=response_timeout,
        ) as bridge:
            bridge.dtr = False
            bridge.rts = False
            transact_exact(bridge, b"AT\r\n", TERMINAL, "AT idle", response_timeout)
            print("FINAL_CARRIER_SOFTAP_STAGE=AT_IDLE")
            transact_exact(
                bridge,
                b"AT+WIFIMODE=2\r\n",
                b"+WIFIMODE: \r\nOK\r\n",
                "WIFIMODE=2",
                response_timeout,
            )
            print("FINAL_CARRIER_SOFTAP_STAGE=WIFIMODE_AP")
            transact_exact(
                bridge,
                b"AT+SOFTAPCONFIG=192.168.42.1,192.168.42.1,255.255.255.0\r\n",
                b"+SOFTAPCONFIG: \r\nOK\r\n",
                "SOFTAPCONFIG",
                response_timeout,
            )
            print("FINAL_CARRIER_SOFTAP_STAGE=SOFTAPCONFIG")
            # These are diagnostic-only ephemeral credentials. Do not log them.
            ssid = "uno-r4-" + os.urandom(4).hex()
            password = os.urandom(18).hex()
            begin_request = (
                "AT+BEGINSOFTAP="
                f"{quote_at_argument(ssid)},{quote_at_argument(password)},1,0\r\n"
            ).encode("ascii")
            transact_exact(
                bridge,
                begin_request,
                AP_RESPONSE,
                "BEGINSOFTAP",
                response_timeout,
            )
            print("FINAL_CARRIER_SOFTAP_STAGE=BEGINSOFTAP_ACCEPTED")

        args.bridge_port = wait_for_watchdog_recovery(
            args.watchdog_recovery_timeout_ms / 1000.0,
            args.wsl_distribution,
        )
        print(f"FINAL_CARRIER_SOFTAP_WATCHDOG_USB_RECOVERED={args.bridge_port}")
        hidraw = ensure_hidraw_access(args.wsl_distribution)
        notification_ab = read_telemetry(
            "--ap-start-notification-ab",
            hidraw,
        )
        read_telemetry("--ap-start-wpa-wait", hidraw)
        read_telemetry("--ap-start-wpa-event-path", hidraw)
        read_telemetry("--ap-start-create-ap", hidraw)
        read_telemetry("--ap-start-allocator-trace", hidraw)
        read_telemetry("--ap-start-allocator-heap-walk", hidraw)
        watchdog = read_telemetry("--ap-start-watchdog", hidraw)
        phase_match = re.search(r"\blast_phase=(\d+)\b", notification_ab)
        if phase_match is None:
            raise RuntimeError(
                "ENABLE_NETWORK event-path A/B telemetry did not report last_phase"
            )
        observed_phase = int(phase_match.group(1))
        if (
            args.expected_notification_ab_last_phase is not None
            and observed_phase != args.expected_notification_ab_last_phase
        ):
            raise RuntimeError(
                "ENABLE_NETWORK event-path A/B telemetry last_phase mismatch: "
                f"expected {args.expected_notification_ab_last_phase}, "
                f"observed {observed_phase}"
            )
        if not WATCHDOG_PATTERN.search(watchdog):
            raise RuntimeError("AP-start watchdog telemetry did not record reset recovery")
        print("FINAL_CARRIER_SOFTAP_WATCHDOG_DIAGNOSTIC_PASS")
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as error:
        primary_error = error
    finally:
        if proxy_uploaded and not args.keep_proxy:
            try:
                args.bridge_port = wait_for_bridge_ready(
                    args.watchdog_recovery_timeout_ms / 1000.0,
                    args.wsl_distribution,
                )
                run_command(
                    [
                        sys.executable,
                        str(DEFAULT_SCAN_RUNNER),
                        "--bridge-port",
                        args.bridge_port,
                        "--arduino-cli",
                        args.arduino_cli,
                    ]
                )
                print("RA4_SCAN_FINAL_CARRIER_RESTORE_PASS")
            except (
                OSError,
                RuntimeError,
                serial.SerialException,
                subprocess.SubprocessError,
            ) as restore_error:
                if primary_error is None:
                    primary_error = restore_error
                else:
                    print("RA4_SCAN_FINAL_CARRIER_RESTORE_FAILED", file=sys.stderr)
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
