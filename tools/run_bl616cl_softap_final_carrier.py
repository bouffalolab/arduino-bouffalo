#!/usr/bin/env python3
"""Validate the no-reset SoftAP lifecycle on the final BL616CL carrier.

The bridge must be built with BL616CL_AP_START_DIAGNOSTIC_WATCHDOG=1. This
runner temporarily installs the RA4M1 Serial/Serial2 AT proxy through the
single final-carrier CDC device, then proves that native SoftAP startup remains
available past the diagnostic watchdog window. Generated AP credentials remain
in process memory and are never printed or written to disk.
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
from run_bl616cl_softap_watchdog_final_carrier import (
    TERMINAL,
    ensure_hidraw_access,
    open_flash_boot_control,
    quote_at_argument,
    read_telemetry,
    require_final_carrier_bridge,
    run_command,
    transact_exact,
    wait_for_bridge_ready,
)


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_PROXY_RUNNER = ROOT_DIR / "tools" / "run_ra4_at_uart_final_carrier.py"
DEFAULT_SCAN_RUNNER = ROOT_DIR / "tools" / "run_ra4_wifis3_scan_final_carrier.py"

AP_RESPONSE = b"+BEGINSOFTAP: 1\r\nOK\r\n"
STATUS_PATTERN = re.compile(rb"^\+GETSTATUS: ([0-9]{1,3})\r\nOK\r\n$")
MAC_PATTERN = re.compile(
    rb"^\+MACSOFTAP: [0-9a-f]{2}(?::[0-9a-f]{2}){5}\r\nOK\r\n$"
)
WATCHDOG_PATTERN = re.compile(
    r"AP_START_WATCHDOG=.*enabled=1 state=2 state_name=disarmed .*persisted=1"
)


def read_status(
    bridge: serial.Serial,
    timeout_seconds: float,
) -> int:
    bridge.reset_input_buffer()
    bridge.write(b"AT+GETSTATUS?\r\n")
    bridge.flush()

    deadline = time.monotonic() + timeout_seconds
    response = bytearray()
    while time.monotonic() < deadline:
        payload = bridge.read(256)
        if payload:
            response.extend(payload)
            if response.endswith(TERMINAL):
                match = STATUS_PATTERN.fullmatch(response)
                if match is None:
                    raise RuntimeError("GETSTATUS did not match expected AT framing")
                return int(match.group(1))
    raise RuntimeError("GETSTATUS timed out")


def wait_for_status(
    bridge: serial.Serial,
    expected: int,
    timeout_seconds: float,
    response_timeout_seconds: float,
) -> tuple[float, list[int]]:
    started_at = time.monotonic()
    deadline = started_at + timeout_seconds
    statuses: list[int] = []
    while time.monotonic() < deadline:
        status = read_status(bridge, response_timeout_seconds)
        if not statuses or statuses[-1] != status:
            statuses.append(status)
        if status == expected:
            return ((time.monotonic() - started_at) * 1000.0, statuses)
        time.sleep(0.25)
    raise RuntimeError(f"SoftAP did not reach GETSTATUS={expected}")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--ftdi-port", default="auto")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--response-timeout-ms", type=int, default=2000)
    parser.add_argument("--status-timeout-ms", type=int, default=20000)
    parser.add_argument("--runtime-settle-ms", type=int, default=1000)
    parser.add_argument(
        "--watchdog-observation-ms",
        type=int,
        default=5000,
        help="time to keep SoftAP active after it reaches listening state",
    )
    parser.add_argument("--wsl-distribution", default="Ubuntu-22.04")
    parser.add_argument(
        "--keep-proxy",
        action="store_true",
        help="leave the temporary RA4M1 AT proxy installed after validation",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if (
        args.response_timeout_ms <= 0
        or args.status_timeout_ms <= 0
        or args.runtime_settle_ms < 0
        or args.watchdog_observation_ms <= 0
    ):
        raise RuntimeError("invalid timeout")

    args.ftdi_port = resolve_ftdi_port(args.ftdi_port)
    response_timeout = args.response_timeout_ms / 1000.0
    status_timeout = args.status_timeout_ms / 1000.0
    proxy_uploaded = False
    ap_started = False
    primary_error: Exception | None = None
    boot_control: serial.Serial | None = None

    print(f"FTDI: {args.ftdi_port} (0403:6001 BOOT/RESET control)")
    print("Target: BL616CL no-reset SoftAP lifecycle; RA4M1 proxy is temporary.")
    print("Privacy: generated SoftAP credentials and raw AT responses stay in memory.")
    try:
        boot_control = open_flash_boot_control(args.ftdi_port)
        print("FINAL_CARRIER_SOFTAP_BOOT_SOURCE=FLASH")
        args.bridge_port = wait_for_bridge_ready(
            status_timeout,
            args.wsl_distribution,
        )
        require_final_carrier_bridge(args.bridge_port)
        print(f"Bridge: {args.bridge_port} (final-carrier BL616CL 2341:1002)")

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
            ap_started = True
            print("FINAL_CARRIER_SOFTAP_STAGE=BEGINSOFTAP_ACCEPTED")

            started_ms, started_statuses = wait_for_status(
                bridge,
                7,
                status_timeout,
                response_timeout,
            )
            transact_exact(
                bridge,
                b"AT+IPSOFTAP\r\n",
                b"+IPSOFTAP: 192.168.42.1\r\nOK\r\n",
                "IPSOFTAP",
                response_timeout,
            )
            expected_ssid = (
                b"+GETSOFTAPSSID: " + ssid.encode("ascii") + b"\r\nOK\r\n"
            )
            transact_exact(
                bridge,
                b"AT+GETSOFTAPSSID?\r\n",
                expected_ssid,
                "GETSOFTAPSSID",
                response_timeout,
            )
            bridge.reset_input_buffer()
            bridge.write(b"AT+MACSOFTAP?\r\n")
            bridge.flush()
            mac_response = bytearray()
            deadline = time.monotonic() + response_timeout
            while time.monotonic() < deadline:
                payload = bridge.read(256)
                if payload:
                    mac_response.extend(payload)
                    if mac_response.endswith(TERMINAL):
                        break
            if not MAC_PATTERN.fullmatch(mac_response):
                raise RuntimeError("MACSOFTAP did not match expected AT framing")
            print("FINAL_CARRIER_SOFTAP_STAGE=AP_LISTENING")

            time.sleep(args.watchdog_observation_ms / 1000.0)
            if read_status(bridge, response_timeout) != 7:
                raise RuntimeError("SoftAP did not remain in GETSTATUS=7 past watchdog window")
            transact_exact(
                bridge,
                b"AT\r\n",
                TERMINAL,
                "AT after watchdog window",
                response_timeout,
            )
            print("FINAL_CARRIER_SOFTAP_STAGE=WATCHDOG_WINDOW_SURVIVED")

            hidraw = ensure_hidraw_access(args.wsl_distribution)
            watchdog = read_telemetry("--ap-start-watchdog", hidraw)
            if not WATCHDOG_PATTERN.search(watchdog):
                raise RuntimeError(
                    "AP-start watchdog telemetry did not report a disarmed "
                    "no-reset SoftAP completion"
                )
            print("FINAL_CARRIER_SOFTAP_STAGE=WATCHDOG_DISARMED")

            transact_exact(
                bridge,
                b"AT+DISCONNECTSOFTAP\r\n",
                TERMINAL,
                "DISCONNECTSOFTAP",
                response_timeout,
            )
            ap_started = False
            stopped_ms, stopped_statuses = wait_for_status(
                bridge,
                0,
                status_timeout,
                response_timeout,
            )

        print(f"FINAL_CARRIER_SOFTAP_START_STATUSES={','.join(map(str, started_statuses))}")
        print(f"FINAL_CARRIER_SOFTAP_STARTED_MS={started_ms:.3f}")
        print(f"FINAL_CARRIER_SOFTAP_STOP_STATUSES={','.join(map(str, stopped_statuses))}")
        print(f"FINAL_CARRIER_SOFTAP_STOPPED_MS={stopped_ms:.3f}")
        print("FINAL_CARRIER_SOFTAP_NO_RESET_LIFECYCLE_PASS")
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
                    status_timeout,
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
