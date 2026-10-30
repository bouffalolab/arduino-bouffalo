#!/usr/bin/env python3
"""Probe paired BL616CL raw-HCI AT transport and restore the WiFiS3 smoke."""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

import serial

from ra4_runtime_uart_route import open_runtime_route
from uno_r4_usb_identity import resolve_paired_bl616cl_port, resolve_relay_port


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_AT_GATE = ROOT_DIR / "tools" / "run_ra4_at_uart_gate.py"
DEFAULT_RESTORE_RUNNER = ROOT_DIR / "tools" / "run_ra4_wifis3_scan_smoke.py"

HCI_BEGIN_REQUEST = b"AT+HCIBEGIN\r\n"
HCI_BEGIN_RESPONSE = b"+HCIBEGIN: OK\r\n"
HCI_RESET_HEADER = b"AT+HCIWRITE=4\r\n"
HCI_RESET_PACKET = b"\x01\x03\x0c\x00"
HCI_RESET_RESPONSE = b"OK\r\n"
HCI_READ_REQUEST = b"AT+HCIREAD\r\n"
HCI_RESET_EVENT = b"\x04\x0e\x04\x01\x03\x0c\x00"
HCI_READ_RESPONSE = b"+HCIREAD: 7|" + HCI_RESET_EVENT + b"OK\r\n"
HCI_END_REQUEST = b"AT+HCIEND\r\n"
HCI_END_RESPONSE = b"+HCIEND: OK\r\n"
HCI_SCAN_DISABLE_HEADER = b"AT+HCIWRITE=6\r\n"
HCI_SCAN_DISABLE_PACKET = b"\x01\x0c\x20\x02\x00\x00"
HCI_SCAN_DISABLE_EVENT = b"\x04\x0e\x04\x01\x0c\x20\x00"
HCI_SCAN_DISABLE_RESPONSE = b"+HCIREAD: 7|" + HCI_SCAN_DISABLE_EVENT + b"OK\r\n"
HCI_SCAN_PARAMETERS_HEADER = b"AT+HCIWRITE=11\r\n"
HCI_SCAN_PARAMETERS_PACKET = b"\x01\x0b\x20\x07\x01\x20\x00\x20\x00\x00\x00"
HCI_SCAN_PARAMETERS_EVENT = b"\x04\x0e\x04\x01\x0b\x20\x00"
HCI_SCAN_PARAMETERS_RESPONSE = b"+HCIREAD: 7|" + HCI_SCAN_PARAMETERS_EVENT + b"OK\r\n"
HCI_SCAN_ENABLE_HEADER = b"AT+HCIWRITE=6\r\n"
HCI_SCAN_ENABLE_PACKET = b"\x01\x0c\x20\x02\x01\x01"


def require_port_identity(port: str, vendor: str, product: str, interface: str) -> None:
    node = Path(port)
    if not node.exists():
        raise RuntimeError(f"serial node does not exist: {port}")
    result = subprocess.run(
        ["udevadm", "info", "--query=property", f"--path=/sys/class/tty/{node.name}"],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"cannot inspect USB identity for {port}: {result.stderr.strip()}")
    properties = dict(
        line.split("=", 1) for line in result.stdout.splitlines() if "=" in line
    )
    actual = (
        properties.get("ID_VENDOR_ID", "").lower(),
        properties.get("ID_MODEL_ID", "").lower(),
        properties.get("ID_USB_INTERFACE_NUM", "").lower(),
    )
    expected = (vendor.lower(), product.lower(), interface.lower())
    if actual != expected:
        raise RuntimeError(
            f"{port} has unexpected USB identity {actual[0]}:{actual[1]} interface "
            f"{actual[2]}; expected {expected[0]}:{expected[1]} interface {expected[2]}"
        )


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    result = subprocess.run(command, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"command failed with exit status {result.returncode}")


def read_exact(link: serial.Serial, expected: bytes, timeout_seconds: float) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while len(received) < len(expected) and time.monotonic() < deadline:
        payload = link.read(len(expected) - len(received))
        if payload:
            received.extend(payload)
    return bytes(received)


def read_hci_read_response(link: serial.Serial, timeout_seconds: float) -> bytes:
    prefix = b"+HCIREAD: "
    actual_prefix = read_exact(link, prefix, timeout_seconds)
    if actual_prefix != prefix:
        raise RuntimeError(
            f"HCIREAD response prefix mismatch: expected {prefix.hex()}, got {actual_prefix.hex()}"
        )

    deadline = time.monotonic() + timeout_seconds
    length_text = bytearray()
    while time.monotonic() < deadline:
        character = link.read(1)
        if not character:
            continue
        if character == b"|":
            break
        if character[0] < ord("0") or character[0] > ord("9"):
            raise RuntimeError(
                f"HCIREAD response length contains non-decimal byte: {character.hex()}"
            )
        length_text.extend(character)
    else:
        raise RuntimeError("HCIREAD response did not contain a payload separator")

    if not length_text:
        raise RuntimeError("HCIREAD response did not contain a payload length")

    payload_length = int(length_text.decode("ascii"))
    payload = read_exact(link, b"\x00" * payload_length, timeout_seconds)
    if len(payload) != payload_length:
        raise RuntimeError(
            f"HCIREAD payload length mismatch: expected {payload_length}, got {len(payload)}"
        )

    suffix = read_exact(link, b"OK\r\n", timeout_seconds)
    if suffix != b"OK\r\n":
        expected_suffix = b"OK\r\n"
        raise RuntimeError(
            f"HCIREAD response suffix mismatch: expected {expected_suffix.hex()}, got {suffix.hex()}"
        )
    return payload


def h4_packet_summary(payload: bytes) -> list[str]:
    summaries: list[str] = []
    index = 0
    while index < len(payload):
        packet_type = payload[index]
        if packet_type == 0x04:
            if index + 3 > len(payload):
                raise RuntimeError("truncated H:4 event header in HCIREAD payload")
            event_code = payload[index + 1]
            parameter_length = payload[index + 2]
            packet_end = index + 3 + parameter_length
            if packet_end > len(payload):
                raise RuntimeError("truncated H:4 event payload in HCIREAD response")
            if event_code == 0x0E and parameter_length >= 4:
                opcode = payload[index + 4] | (payload[index + 5] << 8)
                summaries.append(
                    f"EVENT_COMMAND_COMPLETE opcode=0x{opcode:04x} status={payload[index + 6]}"
                )
            elif event_code == 0x0F and parameter_length >= 4:
                opcode = payload[index + 4] | (payload[index + 5] << 8)
                summaries.append(
                    f"EVENT_COMMAND_STATUS opcode=0x{opcode:04x} status={payload[index + 3]}"
                )
            elif event_code == 0x3E and parameter_length >= 1:
                summaries.append(f"EVENT_LE_META subevent=0x{payload[index + 3]:02x}")
            else:
                summaries.append(
                    f"EVENT code=0x{event_code:02x} parameter_bytes={parameter_length}"
                )
            index = packet_end
        elif packet_type == 0x02:
            if index + 5 > len(payload):
                raise RuntimeError("truncated H:4 ACL header in HCIREAD payload")
            packet_length = payload[index + 3] | (payload[index + 4] << 8)
            packet_end = index + 5 + packet_length
            if packet_end > len(payload):
                raise RuntimeError("truncated H:4 ACL payload in HCIREAD response")
            summaries.append(f"ACL_DATA bytes={packet_length}")
            index = packet_end
        else:
            raise RuntimeError(
                f"unexpected H:4 packet type 0x{packet_type:02x} in HCIREAD response"
            )
    return summaries


def expect_hci_read_contains_command_complete(
    bridge: serial.Serial,
    reference: serial.Serial,
    opcode: int,
    timeout_seconds: float,
    label: str,
) -> None:
    reference.reset_input_buffer()
    bridge.write(HCI_READ_REQUEST)
    bridge.flush()
    payload = read_hci_read_response(reference, timeout_seconds)
    summaries = h4_packet_summary(payload)
    print(f"{label}_PAYLOAD_BYTES={len(payload)}")
    for summary in summaries:
        print(f"{label}_{summary}")
    expected = f"EVENT_COMMAND_COMPLETE opcode=0x{opcode:04x} status=0"
    if expected not in summaries:
        raise RuntimeError(
            f"{label} did not contain successful Command Complete for opcode 0x{opcode:04x}"
        )
    print(f"{label}=PASS")


def expect_response(
    bridge: serial.Serial,
    reference: serial.Serial,
    request: bytes,
    expected: bytes,
    timeout_seconds: float,
    label: str,
) -> None:
    reference.reset_input_buffer()
    bridge.write(request)
    bridge.flush()
    actual = read_exact(reference, expected, timeout_seconds)
    print(f"{label}_RESPONSE_BYTES={len(actual)}")
    if actual != expected:
        raise RuntimeError(
            f"{label} response mismatch: expected {expected.hex()}, got {actual.hex()}"
        )
    print(f"{label}=PASS")


def upload_proxy(args: argparse.Namespace) -> None:
    run_command(
        [
            sys.executable,
            str(DEFAULT_AT_GATE),
            "--bridge-port",
            args.bridge_port,
            "--relay-port",
            args.relay_port,
            "--arduino-cli",
            args.arduino_cli,
            "--runtime-settle-ms",
            str(args.proxy_settle_ms),
            "--skip-status-check",
        ]
    )


def restore_wifis3_scan(args: argparse.Namespace) -> None:
    run_command(
        [
            sys.executable,
            str(DEFAULT_RESTORE_RUNNER),
            "--bridge-port",
            args.bridge_port,
            "--relay-port",
            args.relay_port,
            "--arduino-cli",
            args.arduino_cli,
        ]
    )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Probe paired BL616CL HCIBEGIN/Reset/HCIREAD/HCIEND and restore WiFiS3."
    )
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--relay-port", default="auto")
    parser.add_argument(
        "--reference-port",
        dest="relay_port",
        default=argparse.SUPPRESS,
        help=argparse.SUPPRESS,
    )
    parser.add_argument("--legacy-reference-port")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--response-timeout-ms", type=int, default=3000)
    parser.add_argument("--event-settle-ms", type=int, default=100)
    parser.add_argument(
        "--scan-settle-ms",
        type=int,
        default=500,
        help="Duration to leave the controller scanning before disabling it.",
    )
    parser.add_argument("--proxy-settle-ms", type=int, default=2000)
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    args.relay_port = resolve_relay_port(args.relay_port)
    if (
        args.response_timeout_ms <= 0
        or args.event_settle_ms < 0
        or args.scan_settle_ms < 0
        or args.proxy_settle_ms < 0
    ):
        raise RuntimeError("response timeout must be positive and settle times non-negative")

    require_port_identity(args.bridge_port, "2341", "1002", "00")
    require_port_identity(args.relay_port, "0403", "6010", "00")
    if args.legacy_reference_port is not None:
        require_port_identity(args.legacy_reference_port, "0403", "6010", "00")
    print("Target: paired RA4M1 application Flash only; BL616CL Flash is untouched.")
    print("Cleanup: WiFiS3 scan Sketch will be restored after the temporary AT proxy.")
    print("Privacy: only fixed HCI Reset command/event bytes are validated.")

    primary_error: BaseException | None = None
    route = None
    try:
        upload_proxy(args)
        route = open_runtime_route(
            args.bridge_port,
            args.relay_port,
            args.legacy_reference_port,
            bridge_timeout=0.05,
            receiver_timeout=0.05,
        )
        bridge = route.bridge
        reference = route.receiver
        try:
            bridge.reset_input_buffer()
            reference.reset_input_buffer()
            timeout_seconds = args.response_timeout_ms / 1000.0

            expect_response(
                bridge, reference, HCI_BEGIN_REQUEST, HCI_BEGIN_RESPONSE, timeout_seconds, "HCIBEGIN"
            )
            expect_response(
                bridge,
                reference,
                HCI_RESET_HEADER + HCI_RESET_PACKET,
                HCI_RESET_RESPONSE,
                timeout_seconds,
                "HCIWRITE_RESET",
            )
            if args.event_settle_ms:
                time.sleep(args.event_settle_ms / 1000.0)
            expect_response(
                bridge, reference, HCI_READ_REQUEST, HCI_READ_RESPONSE, timeout_seconds, "HCIREAD_RESET"
            )
            expect_response(
                bridge,
                reference,
                HCI_SCAN_DISABLE_HEADER + HCI_SCAN_DISABLE_PACKET,
                HCI_RESET_RESPONSE,
                timeout_seconds,
                "HCIWRITE_SCAN_DISABLE",
            )
            expect_response(
                bridge,
                reference,
                HCI_READ_REQUEST,
                HCI_SCAN_DISABLE_RESPONSE,
                timeout_seconds,
                "HCIREAD_IDLE_SCAN_DISABLE_COMPAT",
            )
            expect_response(
                bridge,
                reference,
                HCI_SCAN_PARAMETERS_HEADER + HCI_SCAN_PARAMETERS_PACKET,
                HCI_RESET_RESPONSE,
                timeout_seconds,
                "HCIWRITE_SCAN_PARAMETERS",
            )
            expect_response(
                bridge,
                reference,
                HCI_READ_REQUEST,
                HCI_SCAN_PARAMETERS_RESPONSE,
                timeout_seconds,
                "HCIREAD_SCAN_PARAMETERS",
            )
            expect_response(
                bridge,
                reference,
                HCI_SCAN_ENABLE_HEADER + HCI_SCAN_ENABLE_PACKET,
                HCI_RESET_RESPONSE,
                timeout_seconds,
                "HCIWRITE_SCAN_ENABLE",
            )
            expect_hci_read_contains_command_complete(
                bridge,
                reference,
                0x200C,
                timeout_seconds,
                "HCIREAD_SCAN_ENABLE",
            )
            if args.scan_settle_ms:
                time.sleep(args.scan_settle_ms / 1000.0)
            expect_response(
                bridge,
                reference,
                HCI_SCAN_DISABLE_HEADER + HCI_SCAN_DISABLE_PACKET,
                HCI_RESET_RESPONSE,
                timeout_seconds,
                "HCIWRITE_SCAN_DISABLE_AFTER_ENABLE",
            )
            expect_hci_read_contains_command_complete(
                bridge,
                reference,
                0x200C,
                timeout_seconds,
                "HCIREAD_SCAN_DISABLE_AFTER_ENABLE",
            )
            expect_response(
                bridge, reference, HCI_END_REQUEST, HCI_END_RESPONSE, timeout_seconds, "HCIEND"
            )
            print("RA4_BLE_HCI_AT_PROBE_PASS")
        finally:
            if route is not None:
                route.close()
    except BaseException as error:
        primary_error = error
    finally:
        try:
            print("RA4_WIFI_S3_SCAN_RESTORE_STARTED")
            restore_wifis3_scan(args)
            print("RA4_WIFI_S3_SCAN_RESTORE_PASS")
        except BaseException as restore_error:
            print("RA4_WIFI_S3_SCAN_RESTORE_FAILED", file=sys.stderr)
            if primary_error is None:
                primary_error = restore_error

    if primary_error is not None:
        raise primary_error
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
