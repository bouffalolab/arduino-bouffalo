#!/usr/bin/env python3
"""Validate BL616CL +IPSTA through the final carrier's private STA network.

The runner rebuilds and flashes the bridge by default, temporarily writes the
RA4M1 AT proxy, then associates BL616CL to the user-private STA network.
It validates the five WiFiS3 +IPSTA selectors and pings the DHCP gateway
without printing credentials, addresses, or raw AT frames. The RA4M1 scan
Sketch is restored on every exit.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

import serial

from run_bl616cl_apsta_gate import (
    DEFAULT_CREDENTIAL_ENV,
    load_private_credentials,
    quote_at_argument,
)
from run_ra4_wifis3_scan_final_carrier import require_final_carrier_bridge
from uno_r4_usb_identity import resolve_paired_bl616cl_port


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_PROXY_RUNNER = ROOT_DIR / "tools" / "run_ra4_at_uart_final_carrier.py"
DEFAULT_SCAN_RESTORE = ROOT_DIR / "tools" / "run_ra4_wifis3_scan_final_carrier.py"
DEFAULT_FLASH_RUNNER = ROOT_DIR / "tools" / "run_bl616cl_final_carrier_flash.py"
DEFAULT_BUILD = ROOT_DIR / "compile-bl616cl-bridge.sh"

TERMINAL = b"OK\r\n"
STATUS_REQUEST = b"AT+GETSTATUS?\r\n"
STATUS_PATTERN = re.compile(rb"^\+GETSTATUS: ([0-9]{1,3})\r\nOK\r\n$")
BEGINSTA_PATTERN = re.compile(rb"^\+BEGINSTA: [0-9]{1,3}\r\nOK\r\n$")
IPSTA_PATTERN = re.compile(
    rb"^\+IPSTA: ([0-9]{1,3}(?:\.[0-9]{1,3}){3})\r\nOK\r\n$"
)
PING_PATTERN = re.compile(rb"^\+PING: (-?[0-9]+)\r\nOK\r\n$")
RECONNECT_PATTERN = re.compile(rb"^\+RECONNECT: ([01])\r\nOK\r\n$")
SCAN_PREFIX = b"+WIFISCAN: "


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def read_until_terminal(
    link: serial.Serial,
    timeout_seconds: float,
    maximum_bytes: int = 512,
) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        payload = link.read(256)
        if payload:
            received.extend(payload)
            if len(received) > maximum_bytes:
                raise RuntimeError("AT response exceeded its byte limit")
            if received.endswith(TERMINAL):
                return bytes(received)
    raise RuntimeError("AT response timed out")


def transact(
    link: serial.Serial,
    request: bytes,
    timeout_seconds: float,
    maximum_bytes: int = 512,
) -> bytes:
    link.reset_input_buffer()
    link.write(request)
    link.flush()
    return read_until_terminal(link, timeout_seconds, maximum_bytes)


def require_exact(response: bytes, expected: bytes, label: str) -> None:
    if response != expected:
        raise RuntimeError(label + " did not match the expected AT framing")


def parse_status(response: bytes) -> int:
    match = STATUS_PATTERN.fullmatch(response)
    if match is None:
        raise RuntimeError("GETSTATUS did not match the expected AT framing")
    return int(match.group(1))


def parse_reconnect(response: bytes) -> bool:
    match = RECONNECT_PATTERN.fullmatch(response)
    if match is None:
        raise RuntimeError("RECONNECT did not match the expected AT framing")
    return match.group(1) == b"1"


def wait_for_connected(
    link: serial.Serial,
    timeout_seconds: float,
    response_timeout_seconds: float,
) -> tuple[float | None, list[int]]:
    started_at = time.monotonic()
    deadline = started_at + timeout_seconds
    statuses: list[int] = []
    while time.monotonic() < deadline:
        status = parse_status(transact(link, STATUS_REQUEST, response_timeout_seconds))
        if not statuses or statuses[-1] != status:
            statuses.append(status)
        if status == 3:
            return ((time.monotonic() - started_at) * 1000.0, statuses)
        time.sleep(0.25)
    return (None, statuses)


def parse_ipsta(response: bytes, selector: int) -> str:
    match = IPSTA_PATTERN.fullmatch(response)
    if match is None:
        raise RuntimeError(f"IPSTA={selector} did not match the expected AT framing")
    address = match.group(1).decode("ascii")
    octets = address.split(".")
    if any(int(octet) > 255 for octet in octets):
        raise RuntimeError(f"IPSTA={selector} returned an invalid IPv4 address")
    return address


def is_zero_address(address: str) -> bool:
    return address == "0.0.0.0"


def find_target_scan_row(response: bytes, ssid: str) -> tuple[int, int] | None:
    if response == TERMINAL:
        return None
    if not response.startswith(SCAN_PREFIX) or not response.endswith(TERMINAL):
        raise RuntimeError("WIFISCAN did not match the expected AT framing")
    _, separator, payload = response[:-len(TERMINAL)].partition(b"|")
    if not separator:
        raise RuntimeError("WIFISCAN did not include its payload delimiter")

    target = ssid.encode("ascii")
    for row in payload.splitlines():
        fields = row.split(b" | ")
        if len(fields) != 5 or fields[0] != target:
            continue
        try:
            return (int(fields[2]), int(fields[3]))
        except ValueError as error:
            raise RuntimeError("WIFISCAN target row has invalid RSSI or channel") from error
    return None


def upload_proxy(args: argparse.Namespace) -> None:
    run_command(
        [
            sys.executable,
            str(DEFAULT_PROXY_RUNNER),
            "--bridge-port",
            args.bridge_port,
            "--arduino-cli",
            args.arduino_cli,
            "--runtime-settle-ms",
            str(args.runtime_settle_ms),
            "--skip-status-check",
            "--keep-proxy",
        ]
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


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--credential-env", type=Path, default=DEFAULT_CREDENTIAL_ENV)
    parser.add_argument("--response-timeout-ms", type=int, default=2000)
    parser.add_argument("--connect-timeout-ms", type=int, default=30000)
    parser.add_argument("--runtime-settle-ms", type=int, default=1000)
    parser.add_argument(
        "--wifi-mode",
        type=int,
        choices=(1, 3),
        default=1,
        help=(
            "AT-visible role used for the association. The default validates "
            "the standalone STA contract; mode 3 is reserved for AP+STA "
            "coverage."
        ),
    )
    parser.add_argument(
        "--skip-bridge-flash",
        action="store_true",
        help="Use only when the current bridge image is known to include +IPSTA.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier_bridge(args.bridge_port)
    if (
        args.response_timeout_ms <= 0
        or args.connect_timeout_ms <= 0
        or args.runtime_settle_ms < 0
    ):
        raise RuntimeError("invalid timeout")

    ssid, password = load_private_credentials(args.credential_env)
    if password is None:
        raise RuntimeError("IPSTA gate requires a private WPA password")
    begin_sta = (
        "AT+BEGINSTA=" + quote_at_argument(ssid) + "," +
        quote_at_argument(password) + "\r\n"
    ).encode("ascii")
    response_timeout = args.response_timeout_ms / 1000.0

    print(f"Bridge: {args.bridge_port} (final-carrier BL616CL 2341:1002)")
    print("Target: BL616CL bridge Flash and temporary RA4M1 application Flash.")
    print("Privacy: STA credentials, IPSTA values, gateway address, and raw AT frames stay in memory.")

    proxy_uploaded = False
    connected = False
    wifi_mode_changed = False
    original_reconnect_enabled: bool | None = None
    primary_error: Exception | None = None
    statuses: list[int] = []
    try:
        if not args.skip_bridge_flash:
            run_command(["/bin/zsh", "-lc", 'PATH="$HOME/.local/bin:$PATH" ./compile-bl616cl-bridge.sh'])
            run_command([sys.executable, str(DEFAULT_FLASH_RUNNER)])

        upload_proxy(args)
        proxy_uploaded = True
        with serial.Serial(
            args.bridge_port,
            baudrate=115200,
            timeout=0.1,
            write_timeout=1.0,
        ) as bridge:
            bridge.dtr = False
            bridge.rts = False
            require_exact(
                transact(
                    bridge,
                    f"AT+WIFIMODE={args.wifi_mode}\r\n".encode("ascii"),
                    response_timeout,
                ),
                b"+WIFIMODE: \r\nOK\r\n",
                f"WIFIMODE={args.wifi_mode}",
            )
            wifi_mode_changed = True
            original_reconnect_enabled = parse_reconnect(
                transact(bridge, b"AT+RECONNECT?\r\n", response_timeout)
            )
            require_exact(
                transact(bridge, b"AT+RECONNECT=0\r\n", response_timeout),
                TERMINAL,
                "RECONNECT=0",
            )
            target_row = find_target_scan_row(
                transact(
                    bridge,
                    b"AT+WIFISCAN\r\n",
                    20.0,
                    maximum_bytes=8192,
                ),
                ssid,
            )
            if target_row is None:
                print("BL616CL_IPSTA_TARGET_SCAN_PRESENT=0")
                raise RuntimeError(
                    "private STA target was absent from the BL616CL scan"
                )
            print("BL616CL_IPSTA_TARGET_SCAN_PRESENT=1")
            print(f"BL616CL_IPSTA_TARGET_SCAN_RSSI={target_row[0]}")
            print(f"BL616CL_IPSTA_TARGET_SCAN_CHANNEL={target_row[1]}")
            response = transact(bridge, begin_sta, response_timeout)
            if not BEGINSTA_PATTERN.fullmatch(response):
                raise RuntimeError("BEGINSTA did not match the expected AT framing")
            connected_elapsed_ms, statuses = wait_for_connected(
                bridge,
                args.connect_timeout_ms / 1000.0,
                response_timeout,
            )
            if connected_elapsed_ms is None:
                raise RuntimeError("BL616CL STA did not reach GETSTATUS=3")
            connected = True

            values = [
                parse_ipsta(
                    transact(
                        bridge,
                        f"AT+IPSTA={selector}\r\n".encode("ascii"),
                        response_timeout,
                    ),
                    selector,
                )
                for selector in range(5)
            ]
            if is_zero_address(values[0]) or is_zero_address(values[1]):
                raise RuntimeError("IPSTA did not return a usable STA address and gateway")
            if is_zero_address(values[2]):
                raise RuntimeError("IPSTA did not return a usable STA netmask")
            print("BL616CL_IPSTA_SELECTORS_PASS=0,1,2,3,4")

            ping_response = transact(
                bridge,
                f"AT+PING=0,{values[1]},64,2\r\n".encode("ascii"),
                5.0,
            )
            ping = PING_PATTERN.fullmatch(ping_response)
            if ping is None or int(ping.group(1)) < 0:
                raise RuntimeError("BL616CL gateway ping did not return a successful RTT")
            print("BL616CL_IPSTA_GATEWAY_PING_PASS")

            require_exact(
                transact(bridge, b"AT+DISCONNECT\r\n", response_timeout),
                b"+DISCONNECT: \r\nOK\r\n",
                "DISCONNECT",
            )
            connected = False
            if args.wifi_mode != 1:
                require_exact(
                    transact(bridge, b"AT+WIFIMODE=1\r\n", response_timeout),
                    b"+WIFIMODE: \r\nOK\r\n",
                    "WIFIMODE=1 cleanup",
                )
            require_exact(
                transact(
                    bridge,
                    (
                        b"AT+RECONNECT=1\r\n"
                        if original_reconnect_enabled
                        else b"AT+RECONNECT=0\r\n"
                    ),
                    response_timeout,
                ),
                TERMINAL,
                "RECONNECT cleanup",
            )
            original_reconnect_enabled = None
            wifi_mode_changed = False

        print(f"BL616CL_IPSTA_STA_CONNECTED_MS={connected_elapsed_ms:.3f}")
        print("BL616CL_IPSTA_FINAL_CARRIER_PASS")
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as error:
        primary_error = error
    finally:
        if connected or original_reconnect_enabled is not None or wifi_mode_changed:
            try:
                with serial.Serial(
                    args.bridge_port,
                    baudrate=115200,
                    timeout=0.1,
                    write_timeout=1.0,
                ) as bridge:
                    bridge.dtr = False
                    bridge.rts = False
                    if connected:
                        require_exact(
                            transact(
                                bridge,
                                b"AT+DISCONNECT\r\n",
                                response_timeout,
                            ),
                            b"+DISCONNECT: \r\nOK\r\n",
                            "DISCONNECT failure cleanup",
                        )
                    if wifi_mode_changed and args.wifi_mode != 1:
                        require_exact(
                            transact(
                                bridge,
                                b"AT+WIFIMODE=1\r\n",
                                response_timeout,
                            ),
                            b"+WIFIMODE: \r\nOK\r\n",
                            "WIFIMODE failure cleanup",
                        )
                    if original_reconnect_enabled is not None:
                        require_exact(
                            transact(
                                bridge,
                                (
                                    b"AT+RECONNECT=1\r\n"
                                    if original_reconnect_enabled
                                    else b"AT+RECONNECT=0\r\n"
                                ),
                                response_timeout,
                            ),
                            TERMINAL,
                            "RECONNECT failure cleanup",
                        )
            except (OSError, RuntimeError, serial.SerialException):
                print("BL616CL_IPSTA_FAILURE_CLEANUP_FAILED", file=sys.stderr)
        if proxy_uploaded:
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
        print(
            "BL616CL_IPSTA_FAILURE_STATUSES=" +
            (",".join(map(str, statuses)) if statuses else "none")
        )
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
