#!/usr/bin/env python3
"""Validate hostname and IPv6 AT controls through the final BL616CL carrier.

The runner loads the private STA credentials only into process memory. It
temporarily replaces the RA4M1 Sketch with the AT proxy and restores the
standard WiFiS3 scan Sketch in every exit path. BL616CL Flash is untouched.
"""

from __future__ import annotations

import argparse
import ipaddress
import re
import subprocess
import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_bl616cl_apsta_gate import DEFAULT_CREDENTIAL_ENV, load_private_credentials
from run_ra4_at_uart_final_carrier import require_final_carrier_bridge
from run_ra4_at_uart_gate import transact_exact_query
import run_ra4_wifis3_scan_final_carrier as final_scan
from uno_r4_usb_identity import resolve_paired_bl616cl_port


ROOT_DIR = Path(__file__).resolve().parents[1]
FQBN = "arduino:renesas_uno:unor4wifi"
PROXY_SKETCH = ROOT_DIR / "examples" / "ra4-at-transcript-proxy"
PROXY_BUILD_DIR = ROOT_DIR / "build" / "ra4-at-transcript-proxy-hostname-ipv6"
PROXY_IMAGE = PROXY_BUILD_DIR / "ra4-at-transcript-proxy.ino.bin"
SAMBA_RUNNER = ROOT_DIR / "tools" / "run_ra4_bossac_final_carrier.py"
TEST_HOSTNAME = "bl616cl-control-gate"

TERMINAL = b"OK\r\n"
WIFI_MODE_REQUEST = b"AT+WIFIMODE?\r\n"
WIFI_MODE_RESPONSE = b"+WIFIMODE: 1\r\n\r\nOK\r\n"
RECONNECT_READ_REQUEST = b"AT+RECONNECT?\r\n"
DISCONNECT_REQUEST = b"AT+DISCONNECT\r\n"
DISCONNECT_RESPONSE = b"+DISCONNECT: \r\nOK\r\n"
STATUS_REQUEST = b"AT+GETSTATUS?\r\n"
IPV6_ENABLE_REQUEST = b"AT+IPV6\r\n"

HOSTNAME_RESPONSE = re.compile(rb"^\+HOSTNAME: ([^\r\n]{1,31})\r\nOK\r\n$")
RECONNECT_RESPONSE = re.compile(rb"^\+RECONNECT: ([01])\r\nOK\r\n$")
STATUS_RESPONSE = re.compile(rb"^\+GETSTATUS: ([0-9]{1,3})\r\nOK\r\n$")
IPV6_RESPONSE = re.compile(rb"^\+IPV6: ([0-9A-Fa-f:.]+)\r\nOK\r\n$")


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def quote_at_argument(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def hostname_write_request(hostname: str) -> bytes:
    return ("AT+HOSTNAME=" + quote_at_argument(hostname) + "\r\n").encode("ascii")


def begin_sta_request(ssid: str, password: str) -> bytes:
    return (
        "AT+BEGINSTA="
        + quote_at_argument(ssid)
        + ","
        + quote_at_argument(password)
        + "\r\n"
    ).encode("ascii")


def read_until_terminal(link: serial.Serial, timeout_seconds: float) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        payload = link.read(256)
        if not payload:
            continue
        received.extend(payload)
        if len(received) > 256:
            raise RuntimeError("AT response exceeded 256 bytes")
        if received.endswith((TERMINAL, b"ERROR\r\n")):
            return bytes(received)
    raise RuntimeError("AT response timed out")


def transact(
    bridge: serial.Serial,
    request: bytes,
    timeout_seconds: float,
) -> bytes:
    bridge.reset_input_buffer()
    bridge.write(request)
    bridge.flush()
    try:
        return read_until_terminal(bridge, timeout_seconds)
    except RuntimeError as error:
        # Only the command token is safe to log; arguments may be credentials.
        token = re.match(rb"AT(?:\+[A-Z0-9_]+)?\??", request)
        label = token.group().decode("ascii") if token else "AT request"
        raise RuntimeError(label + ": " + str(error)) from error


def require_exact(response: bytes, expected: bytes, label: str) -> None:
    if response != expected:
        raise RuntimeError(f"{label} did not match its expected AT framing")


def read_hostname(bridge: serial.Serial, timeout_seconds: float) -> str:
    match = HOSTNAME_RESPONSE.fullmatch(
        transact(bridge, b"AT+HOSTNAME?\r\n", timeout_seconds)
    )
    if match is None:
        raise RuntimeError("HOSTNAME readback did not match its expected AT framing")
    return match.group(1).decode("ascii", "strict")


def read_reconnect(bridge: serial.Serial, timeout_seconds: float) -> bool:
    match = RECONNECT_RESPONSE.fullmatch(
        transact(bridge, RECONNECT_READ_REQUEST, timeout_seconds)
    )
    if match is None:
        raise RuntimeError("RECONNECT readback did not match its expected AT framing")
    return match.group(1) == b"1"


def set_reconnect(
    bridge: serial.Serial,
    enabled: bool,
    timeout_seconds: float,
) -> None:
    request = b"AT+RECONNECT=1\r\n" if enabled else b"AT+RECONNECT=0\r\n"
    require_exact(transact(bridge, request, timeout_seconds), TERMINAL, "RECONNECT write")


def read_status(bridge: serial.Serial, timeout_seconds: float) -> int:
    match = STATUS_RESPONSE.fullmatch(
        transact(bridge, STATUS_REQUEST, timeout_seconds)
    )
    if match is None:
        raise RuntimeError("GETSTATUS did not match its expected AT framing")
    status = int(match.group(1))
    if status > 255:
        raise RuntimeError("GETSTATUS contained an out-of-range status")
    return status


def wait_for_status(
    bridge: serial.Serial,
    expected_status: int,
    timeout_seconds: float,
    response_timeout_seconds: float,
) -> None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        if read_status(bridge, response_timeout_seconds) == expected_status:
            return
        time.sleep(0.25)
    raise RuntimeError(f"GETSTATUS did not reach {expected_status}")


def wait_for_link_local_ipv6(
    bridge: serial.Serial,
    timeout_seconds: float,
    response_timeout_seconds: float,
) -> None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        match = IPV6_RESPONSE.fullmatch(
            transact(bridge, b"AT+IPV6?\r\n", response_timeout_seconds)
        )
        if match is not None:
            try:
                address = ipaddress.IPv6Address(match.group(1).decode("ascii"))
            except ipaddress.AddressValueError:
                address = None
            if address is not None and address.is_link_local:
                return
        time.sleep(0.25)
    raise RuntimeError("IPV6 did not return a valid link-local address")


def verify_active_reconnect(bridge, ssid, password, response_timeout_seconds,
                            connect_timeout_seconds, ipv6_timeout_seconds, cycle):
    """Exercise a requested STA disconnect/join, without controlling the AP."""
    for enabled in (True, False):
        set_reconnect(bridge, enabled, response_timeout_seconds)
        if read_reconnect(bridge, response_timeout_seconds) != enabled:
            raise RuntimeError("RECONNECT setting readback mismatch")
    print(f"BL616CL_RECONNECT_POLICY_READBACK_PASS={cycle}")
    require_exact(transact(bridge, DISCONNECT_REQUEST, response_timeout_seconds),
                  DISCONNECT_RESPONSE, "active reconnect DISCONNECT")
    wait_for_status(bridge, 0, 10.0, response_timeout_seconds)
    response = transact(bridge, begin_sta_request(ssid, password), response_timeout_seconds)
    if not re.fullmatch(rb"\+BEGINSTA: [0-9]{1,3}\r\nOK\r\n", response):
        raise RuntimeError("active reconnect BEGINSTA framing mismatch")
    wait_for_status(bridge, 3, connect_timeout_seconds, response_timeout_seconds)
    if read_hostname(bridge, response_timeout_seconds) != TEST_HOSTNAME:
        raise RuntimeError("HOSTNAME lost after active reconnect")
    require_exact(transact(bridge, IPV6_ENABLE_REQUEST, response_timeout_seconds),
                  TERMINAL, "active reconnect IPV6 enable")
    wait_for_link_local_ipv6(bridge, ipv6_timeout_seconds, response_timeout_seconds)
    print(f"BL616CL_ACTIVE_STA_RECONNECT_IPV6_PASS={cycle}")


def compile_proxy(args: argparse.Namespace) -> Path:
    run_command(
        [
            args.arduino_cli,
            "compile",
            "--fqbn",
            FQBN,
            "--build-path",
            str(PROXY_BUILD_DIR),
            str(PROXY_SKETCH),
        ]
    )
    if not PROXY_IMAGE.is_file():
        raise RuntimeError("compiled RA4M1 AT proxy image does not exist")
    return PROXY_IMAGE


def upload_proxy(args: argparse.Namespace, image: Path) -> None:
    run_command(
        [
            sys.executable,
            str(SAMBA_RUNNER),
            "--bridge-port",
            args.bridge_port,
            "--image",
            str(image),
            "--runtime-settle-ms",
            str(args.runtime_settle_ms),
            "--attempts",
            "3",
        ]
    )


def restore_scan(args: argparse.Namespace) -> None:
    run_command(
        [
            sys.executable,
            str(final_scan.__file__),
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
    parser.add_argument("--credential-env", type=Path, default=DEFAULT_CREDENTIAL_ENV)
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--response-timeout-ms", type=int, default=10000)
    parser.add_argument("--connect-timeout-ms", type=int, default=30000)
    parser.add_argument("--ipv6-timeout-ms", type=int, default=15000)
    parser.add_argument("--runtime-settle-ms", type=int, default=1000)
    parser.add_argument("--reconnect-cycles", type=int, default=0,
                        help="0..3 active STA disconnect/rejoin cycles; not passive AP loss")
    parser.add_argument("--ipv6-tcp-data", action="store_true",
                        help="Require the optional IPv6 diagnostic firmware and run exact TCP echo.")
    parser.add_argument(
        "--source-only",
        action="store_true",
        help="Compile the temporary RA4M1 proxy without using hardware.",
    )
    return parser.parse_args()


def main(prepared_ipv6_peer=None) -> int:
    args = parse_arguments()
    if (
        args.response_timeout_ms <= 0
        or args.connect_timeout_ms <= 0
        or args.ipv6_timeout_ms <= 0
        or args.runtime_settle_ms < 0
        or not 0 <= args.reconnect_cycles <= 3
    ):
        raise RuntimeError("invalid timeout")

    if args.source_only:
        compile_proxy(args)
        print("BL616CL_HOSTNAME_IPV6_SOURCE_PASS")
        return 0

    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier_bridge(args.bridge_port)
    ssid, password = load_private_credentials(args.credential_env)
    if password is None:
        raise RuntimeError("hostname/IPv6 gate requires a private WPA password")

    print(f"Bridge: {args.bridge_port} (final-carrier BL616CL 2341:1002)")
    print("Target: RA4M1 application Flash only; BL616CL Flash is untouched.")
    print("Privacy: STA credentials, hostnames, and IPv6 addresses remain in memory.")

    bridge = None
    proxy_uploaded = False
    original_hostname = None
    original_reconnect = None
    station_requested = False
    station_disconnected = False
    primary_error: Exception | None = None
    response_timeout_seconds = args.response_timeout_ms / 1000.0
    try:
        if args.ipv6_tcp_data and prepared_ipv6_peer is None:
            from bl616cl_ipv6_tcp_windows_peer import PreparedIPv6Peer
            prepared_ipv6_peer = PreparedIPv6Peer()
        image = compile_proxy(args)
        upload_proxy(args, image)
        proxy_uploaded = True
        bridge = serial.Serial(
            args.bridge_port,
            baudrate=115200,
            timeout=0.1,
            write_timeout=1.0,
        )
        bridge.dtr = False
        bridge.rts = False
        time.sleep(args.runtime_settle_ms / 1000.0)

        preflight = transact_exact_query(
            bridge, bridge, b"AT\r\n", TERMINAL, response_timeout_seconds,
        )
        if preflight != TERMINAL:
            # This exchange precedes all private requests. Only byte count and
            # equality are logged; never include raw UART contents.
            print(f"BL616CL_AT_PREFLIGHT_MISMATCH_BYTES={len(preflight)}")
        require_exact(preflight, TERMINAL, "AT proxy preflight")
        require_exact(
            transact(bridge, WIFI_MODE_REQUEST, response_timeout_seconds),
            WIFI_MODE_RESPONSE,
            "WIFIMODE preflight",
        )
        original_hostname = read_hostname(bridge, response_timeout_seconds)
        original_reconnect = read_reconnect(bridge, response_timeout_seconds)

        require_exact(
            transact(
                bridge,
                hostname_write_request(TEST_HOSTNAME),
                response_timeout_seconds,
            ),
            b"+HOSTNAME: \r\nOK\r\n",
            "HOSTNAME write",
        )
        if read_hostname(bridge, response_timeout_seconds) != TEST_HOSTNAME:
            raise RuntimeError("HOSTNAME immediate readback did not retain the test value")
        print("BL616CL_HOSTNAME_WRITE_READBACK_PASS")

        set_reconnect(bridge, False, response_timeout_seconds)
        station_requested = True
        begin_response = transact(
            bridge,
            begin_sta_request(ssid, password),
            response_timeout_seconds,
        )
        if not re.fullmatch(rb"\+BEGINSTA: [0-9]{1,3}\r\nOK\r\n", begin_response):
            raise RuntimeError("BEGINSTA did not match its expected AT framing")
        wait_for_status(
            bridge,
            expected_status=3,
            timeout_seconds=args.connect_timeout_ms / 1000.0,
            response_timeout_seconds=response_timeout_seconds,
        )
        if read_hostname(bridge, response_timeout_seconds) != TEST_HOSTNAME:
            raise RuntimeError("HOSTNAME was not retained after STA netif creation")
        print("BL616CL_HOSTNAME_STA_RECONNECT_READBACK_PASS")

        require_exact(
            transact(bridge, IPV6_ENABLE_REQUEST, response_timeout_seconds),
            TERMINAL,
            "IPV6 enable",
        )
        wait_for_link_local_ipv6(
            bridge,
            timeout_seconds=args.ipv6_timeout_ms / 1000.0,
            response_timeout_seconds=response_timeout_seconds,
        )
        print("BL616CL_IPV6_LINK_LOCAL_PASS")
        for cycle in range(1, args.reconnect_cycles + 1):
            verify_active_reconnect(
                bridge, ssid, password, response_timeout_seconds,
                args.connect_timeout_ms / 1000.0, args.ipv6_timeout_ms / 1000.0, cycle)

        if args.ipv6_tcp_data:
            match = IPV6_RESPONSE.fullmatch(transact(bridge, b"AT+IPV6?\r\n", response_timeout_seconds))
            if match is None:
                raise RuntimeError("IPv6 TCP address query framing mismatch")
            from inspect_bl616cl_ipv6_tcp import log_snapshot
            log_snapshot()
            try:
                prepared_ipv6_peer.exchange(match.group(1).decode("ascii"))
            finally:
                log_snapshot()

        require_exact(
            transact(bridge, DISCONNECT_REQUEST, response_timeout_seconds),
            DISCONNECT_RESPONSE,
            "DISCONNECT",
        )
        wait_for_status(
            bridge,
            expected_status=0,
            timeout_seconds=10.0,
            response_timeout_seconds=response_timeout_seconds,
        )
        station_disconnected = True
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as error:
        primary_error = error
    finally:
        if prepared_ipv6_peer is not None:
            prepared_ipv6_peer.close()
        if bridge is not None and bridge.is_open:
            if station_requested and not station_disconnected:
                try:
                    require_exact(
                        transact(
                            bridge,
                            DISCONNECT_REQUEST,
                            response_timeout_seconds,
                        ),
                        DISCONNECT_RESPONSE,
                        "cleanup DISCONNECT",
                    )
                except (OSError, RuntimeError, serial.SerialException):
                    print("BL616CL_HOSTNAME_IPV6_CLEANUP_DISCONNECT_FAILED", file=sys.stderr)
            if original_hostname is not None:
                try:
                    require_exact(
                        transact(
                            bridge,
                            hostname_write_request(original_hostname),
                            response_timeout_seconds,
                        ),
                        b"+HOSTNAME: \r\nOK\r\n",
                        "cleanup HOSTNAME",
                    )
                    print("BL616CL_HOSTNAME_RESTORED")
                except (OSError, RuntimeError, serial.SerialException):
                    print("BL616CL_HOSTNAME_RESTORE_FAILED", file=sys.stderr)
            if original_reconnect is not None:
                try:
                    set_reconnect(bridge, original_reconnect, response_timeout_seconds)
                    print("BL616CL_RECONNECT_RESTORED")
                except (OSError, RuntimeError, serial.SerialException):
                    print("BL616CL_RECONNECT_RESTORE_FAILED", file=sys.stderr)
            bridge.close()
        if proxy_uploaded:
            try:
                restore_scan(args)
            except (
                OSError,
                RuntimeError,
                serial.SerialException,
                subprocess.SubprocessError,
            ):
                print("RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_FAILED", file=sys.stderr)
                if primary_error is None:
                    primary_error = RuntimeError("RA4M1 scan restore failed")

    if primary_error is not None:
        raise primary_error
    print("BL616CL_HOSTNAME_IPV6_FINAL_CARRIER_PASS")
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
