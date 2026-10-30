#!/usr/bin/env python3
"""Program the paired RA4M1 over the direct BL616CL GPIO34/GPIO35 UART path."""

from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
import time
from pathlib import Path

import serial

from uno_r4_usb_identity import resolve_paired_bl616cl_port, resolve_relay_port
from upload_ra4_samba_split import (
    APPLICATION_FLASH_ADDRESS,
    FLASH_END_ADDRESS,
    FLASH_PAGE_SIZE,
    WORD_COPY_APPLET,
    WRITE_BUFFER_ADDRESS,
    WRITE_BUFFER_SIZE,
    capture,
    command,
    hex_bytes,
    open_serial,
    read_exact,
    samba_checksum,
    write_all,
    write_paced,
)
from ra4_relay_route import select_ra4_runtime_route


WORD_COPY_STACK_ADDRESS = 0x00000020
WORD_COPY_WORDS_ADDRESS = 0x00000030


def require_port_identity(port: str, vendor: str, product: str, interface: str) -> None:
    device_path = Path(port)
    if not device_path.exists():
        raise RuntimeError(f"serial node does not exist: {port}")

    result = subprocess.run(
        [
            "udevadm",
            "info",
            "--query=property",
            f"--path=/sys/class/tty/{device_path.name}",
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"cannot inspect USB identity for {port}: {result.stderr.strip()}")

    properties = dict(
        line.split("=", 1) for line in result.stdout.splitlines() if "=" in line
    )
    actual_vendor = properties.get("ID_VENDOR_ID", "").lower()
    actual_product = properties.get("ID_MODEL_ID", "").lower()
    actual_interface = properties.get("ID_USB_INTERFACE_NUM", "").lower()
    if actual_vendor != vendor.lower() or actual_product != product.lower():
        raise RuntimeError(
            f"{port} has unexpected USB identity: "
            f"{actual_vendor or '<missing>'}:{actual_product or '<missing>'}; "
            f"expected {vendor}:{product}"
        )
    if actual_interface != interface.lower():
        raise RuntimeError(
            f"{port} has unexpected USB interface: "
            f"{actual_interface or '<missing>'}; expected {interface}"
        )


def initialize_samba(
    link: serial.Serial,
    timeout: float,
    *,
    require_version: bool = True,
) -> str | None:
    time.sleep(0.1)
    for index in range(1, 3):
        link.reset_input_buffer()
        write_all(link, b"\x80")
        reply = capture(link, timeout, 16)
        print(f"SAMBA_AUTO_BAUD_{index}={hex_bytes(reply)}")

    link.reset_input_buffer()
    write_all(link, b"#")
    reply = capture(link, timeout, 16)
    print(f"SAMBA_AUTO_BAUD_CONFIRM={hex_bytes(reply)}")

    link.reset_input_buffer()
    write_all(link, b"N#")
    reply = read_exact(link, 2, timeout, "SAM-BA binary mode")
    if reply != b"\n\r":
        raise RuntimeError(f"SAM-BA binary mode returned unexpected reply {hex_bytes(reply)}")
    print(f"SAMBA_BINARY_MODE={hex_bytes(reply)}")

    link.reset_input_buffer()
    write_all(link, b"V#")
    version = capture(link, timeout, 128).decode("ascii", errors="replace").strip()
    if not version.startswith("Arduino Bootloader (SAM-BA extended) "):
        print(f"SAMBA_VERSION_UNAVAILABLE={version!r}")
        if require_version:
            raise RuntimeError(f"unexpected SAM-BA version reply: {version!r}")
        return None
    print(f"SAMBA_VERSION={version}")
    return version


def restore_user_uart_baud(
    port: str,
    timeout: float,
    settle_seconds: float,
) -> None:
    """Force a normal CDC transition after a failed bootstrap attempt."""
    link = open_serial(port, 9600, timeout)
    try:
        # A two-step transition avoids relying on the host CDC driver's
        # remembered termios state after an interrupted SAM-BA session.
        time.sleep(0.05)
        link.baudrate = 115200
        time.sleep(settle_seconds)
        link.reset_input_buffer()
        print("CDC_FAILURE_RECOVERY=9600_TO_115200")
    finally:
        link.close()


def expect_response(
    link: serial.Serial,
    payload: bytes,
    expected: int,
    timeout: float,
    description: str,
) -> None:
    link.reset_input_buffer()
    write_all(link, payload)
    reply = read_exact(link, 3, timeout, description)
    expected_reply = bytes((expected, 0x0A, 0x0D))
    if reply != expected_reply:
        raise RuntimeError(f"{description} returned unexpected reply {hex_bytes(reply)}")


def send_samba_write(
    link: serial.Serial,
    address: int,
    payload: bytes,
    packet_bytes: int,
    delay_seconds: float,
    description: str,
) -> None:
    link.reset_input_buffer()
    write_all(link, command("S", address, len(payload)))
    time.sleep(0.002)
    write_paced(link, payload, packet_bytes, delay_seconds)
    packet_count = (len(payload) + packet_bytes - 1) // packet_bytes
    print(
        f"{description} direct_payload_bytes={len(payload)} packets={packet_count} "
        f"packet_bytes={packet_bytes} delay_ms={int(delay_seconds * 1000)}"
    )


def send_samba_word(
    link: serial.Serial,
    address: int,
    value: int,
    description: str,
) -> None:
    """Configure the RAM word-copy applet using SAM-BA's no-reply W command."""
    write_all(link, f"W{address:08X},{value:08X}#".encode("ascii"))
    time.sleep(0.01)
    print(f"{description} address=0x{address:08X} value=0x{value:08X}")


def verify_flash(link: serial.Serial, image: bytes, timeout: float) -> None:
    for chunk_index, offset in enumerate(range(0, len(image), WRITE_BUFFER_SIZE)):
        payload = image[offset : offset + WRITE_BUFFER_SIZE]
        flash_address = APPLICATION_FLASH_ADDRESS + offset
        link.reset_input_buffer()
        write_all(link, command("Z", flash_address, len(payload)))
        reply = read_exact(
            link,
            12,
            timeout,
            f"Flash chunk {chunk_index} checksum 0x{flash_address:08X}",
        )
        if reply[0:1] != b"Z" or reply[9:] != b"#\n\r":
            raise RuntimeError(
                f"Flash chunk {chunk_index} checksum returned unexpected reply "
                f"{hex_bytes(reply)}"
            )
        target_checksum = int(reply[1:9].decode("ascii"), 16)
        expected_checksum = samba_checksum(payload)
        if target_checksum != expected_checksum:
            raise RuntimeError(
                f"Flash chunk {chunk_index} checksum mismatch at 0x{flash_address:08X}: "
                f"target=0x{target_checksum:04X} expected=0x{expected_checksum:04X}"
            )
        print(
            f"RA4_FLASH_CHECKSUM_PASS index={chunk_index} "
            f"address=0x{flash_address:08X} bytes={len(payload)} "
            f"crc16=0x{target_checksum:04X}"
        )
    print("RA4_FLASH_CONTENT_VERIFY_PASS")


def capture_until_expected(
    link: serial.Serial,
    timeout: float,
    maximum_bytes: int,
    expected_markers: list[str],
) -> str:
    deadline = time.monotonic() + timeout
    received = bytearray()
    while time.monotonic() < deadline:
        payload = link.read(512)
        if payload:
            received.extend(payload)
            if len(received) > maximum_bytes:
                raise RuntimeError(f"runtime capture exceeded {maximum_bytes} bytes")
            text = received.decode("ascii", errors="replace").replace("\x00", "")
            if all(marker in text for marker in expected_markers):
                return text
    return received.decode("ascii", errors="replace").replace("\x00", "")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--relay-port", default="auto")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--bootstrap-settle-ms", type=int, default=500)
    parser.add_argument("--runtime-settle-ms", type=int, default=300)
    parser.add_argument(
        "--attempts",
        type=int,
        default=1,
        help="Bounded full-image attempts; every failed attempt restores CDC 115200.",
    )
    parser.add_argument(
        "--retry-settle-ms",
        type=int,
        default=500,
        help="Delay between failed full-image attempts.",
    )
    parser.add_argument(
        "--relay-settle-ms",
        type=int,
        default=500,
        help="Known-route settle time after selecting the RA4M1 relay path.",
    )
    parser.add_argument("--read-timeout-ms", type=int, default=1000)
    parser.add_argument("--long-read-timeout-ms", type=int, default=5000)
    parser.add_argument("--payload-packet-bytes", type=int, default=64)
    parser.add_argument("--payload-interpacket-delay-ms", type=int, default=5)
    parser.add_argument("--verify-crc16", action="store_true")
    parser.add_argument("--runtime-capture-ms", type=int, default=45000)
    parser.add_argument("--maximum-capture-bytes", type=int, default=16384)
    parser.add_argument(
        "--skip-runtime-capture",
        action="store_true",
        help="Start the uploaded Sketch and return after the 115200 CDC handoff.",
    )
    parser.add_argument(
        "--expect",
        action="append",
        default=[],
        help="Require this marker in the post-upload runtime capture. Repeatable.",
    )
    return parser.parse_args()


def retry_self(args: argparse.Namespace) -> int:
    child_arguments: list[str] = []
    skip_next = False
    for argument in sys.argv[1:]:
        if skip_next:
            skip_next = False
            continue
        if argument in ("--attempts", "--retry-settle-ms"):
            skip_next = True
            continue
        if argument.startswith("--attempts=") or argument.startswith("--retry-settle-ms="):
            continue
        child_arguments.append(argument)

    last_status = 1
    for attempt in range(1, args.attempts + 1):
        print(f"RA4_DIRECT_SAMBA_UPLOAD_ATTEMPT={attempt}/{args.attempts}")
        result = subprocess.run(
            [sys.executable, str(Path(__file__).resolve()), *child_arguments],
            check=False,
        )
        if result.returncode == 0:
            return 0
        last_status = result.returncode
        if attempt < args.attempts:
            time.sleep(args.retry_settle_ms / 1000)
    return last_status


def main() -> int:
    args = parse_arguments()
    if args.attempts <= 0 or args.retry_settle_ms < 0:
        raise RuntimeError("attempt count and retry delay must be valid")
    if args.attempts > 1:
        return retry_self(args)

    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    args.relay_port = resolve_relay_port(args.relay_port)
    if (
        args.bootstrap_settle_ms <= 0
        or args.runtime_settle_ms < 0
        or args.read_timeout_ms <= 0
        or args.long_read_timeout_ms <= 0
        or args.payload_packet_bytes <= 0
        or args.payload_interpacket_delay_ms < 0
        or args.runtime_capture_ms <= 0
        or args.maximum_capture_bytes <= 0
        or args.relay_settle_ms < 0
    ):
        raise RuntimeError("invalid timeout, capture size, or payload pacing argument")

    image_path = args.image.resolve()
    image = image_path.read_bytes()
    if not image:
        raise RuntimeError("compiled RA4M1 image is empty")
    if APPLICATION_FLASH_ADDRESS + len(image) > FLASH_END_ADDRESS:
        raise RuntimeError("image exceeds the RA4M1 application Flash range")
    if len(WORD_COPY_APPLET) != WRITE_BUFFER_ADDRESS:
        raise RuntimeError("BOSSA compatibility staging layout is inconsistent")
    if WRITE_BUFFER_SIZE % 4:
        raise RuntimeError("BOSSA staging buffer size must be word aligned")

    require_port_identity(args.bridge_port, "2341", "1002", "00")
    require_port_identity(args.relay_port, "0403", "6010", "00")
    print(f"Bridge:    {args.bridge_port} (2341:1002 interface 00)")
    print(f"Relay:     {args.relay_port} (0403:6010 interface 00)")
    print(f"Image:     {image_path}")
    print(f"Image size: {len(image)}")
    print(f"Image SHA256: {hashlib.sha256(image).hexdigest()}")
    print("Target: RA4M1 application Flash only; BL616CL and ESP32-S3 Flash are untouched.")
    print("Relay route: RA4M1 UART (RTS=False, physical RTS# HIGH, relay released).")

    relay = serial.Serial(args.relay_port, 115200, timeout=0.2, write_timeout=0.2)
    relay.dtr = False
    relay.rts = False
    bridge: serial.Serial | None = None
    samba_may_be_active = False
    runtime_requested = False
    try:
        select_ra4_runtime_route(relay, args.relay_settle_ms)
        print(
            "RELAY_RUNTIME_ROUTE="
            f"known-state settle_ms={args.relay_settle_ms}"
        )
        bridge = open_serial(args.bridge_port, 115200, args.read_timeout_ms / 1000)
        bridge.baudrate = 1200
        print("CDC_SET_LINE_CODING=1200")
        time.sleep(args.bootstrap_settle_ms / 1000)
        bridge.close()

        bridge = open_serial(args.bridge_port, 230400, args.read_timeout_ms / 1000)
        samba_may_be_active = True
        initialize_samba(
            bridge,
            args.read_timeout_ms / 1000,
            require_version=False,
        )

        delay_seconds = args.payload_interpacket_delay_ms / 1000
        send_samba_write(
            bridge,
            0,
            WORD_COPY_APPLET,
            args.payload_packet_bytes,
            delay_seconds,
            "BOSSA compatibility applet",
        )
        send_samba_word(
            bridge,
            WORD_COPY_WORDS_ADDRESS,
            WRITE_BUFFER_SIZE // 4,
            "BOSSA compatibility applet words",
        )
        send_samba_word(
            bridge,
            WORD_COPY_STACK_ADDRESS,
            0,
            "BOSSA compatibility applet stack",
        )
        expect_response(
            bridge,
            f"X{APPLICATION_FLASH_ADDRESS:08X}#".encode("ascii"),
            ord("X"),
            args.long_read_timeout_ms / 1000,
            "RA4M1 application Flash erase",
        )
        print(f"RA4_FLASH_ERASED_FROM=0x{APPLICATION_FLASH_ADDRESS:08X}")

        flash_address = APPLICATION_FLASH_ADDRESS
        for chunk_index, offset in enumerate(range(0, len(image), WRITE_BUFFER_SIZE)):
            payload = image[offset : offset + WRITE_BUFFER_SIZE]
            program_size = (
                (len(payload) + FLASH_PAGE_SIZE - 1) // FLASH_PAGE_SIZE * FLASH_PAGE_SIZE
            )
            staged = payload.ljust(program_size, b"\x00")
            send_samba_write(
                bridge,
                WRITE_BUFFER_ADDRESS,
                staged,
                args.payload_packet_bytes,
                delay_seconds,
                f"Flash chunk {chunk_index} staging",
            )
            expect_response(
                bridge,
                f"Y{WRITE_BUFFER_ADDRESS:08X},0#".encode("ascii"),
                ord("Y"),
                args.read_timeout_ms / 1000,
                f"Flash chunk {chunk_index} select staging buffer",
            )
            expect_response(
                bridge,
                command("Y", flash_address, program_size),
                ord("Y"),
                args.long_read_timeout_ms / 1000,
                f"Flash chunk {chunk_index} write 0x{flash_address:08X}",
            )
            print(
                f"RA4_FLASH_CHUNK_PASS index={chunk_index} address=0x{flash_address:08X} "
                f"programmed={program_size} payload={len(payload)}"
            )
            flash_address += program_size
        print("RA4_FLASH_WRITE_COMMAND_PASS")

        if args.verify_crc16:
            verify_flash(bridge, image, args.long_read_timeout_ms / 1000)

        bridge.reset_input_buffer()
        write_all(bridge, b"K#")
        runtime_requested = True
        print("SAMBA_RESET_COMMAND=K#")
        time.sleep(0.05)
        bridge.baudrate = 115200
        time.sleep(args.runtime_settle_ms / 1000)
        if not args.skip_runtime_capture:
            runtime = capture_until_expected(
                bridge,
                args.runtime_capture_ms / 1000,
                args.maximum_capture_bytes,
                args.expect,
            )
            print("RA4_RUNTIME_CAPTURE_BEGIN")
            print(runtime, end="" if runtime.endswith("\n") else "\n")
            print("RA4_RUNTIME_CAPTURE_END")
            for marker in args.expect:
                if marker not in runtime:
                    raise RuntimeError(
                        f"required runtime marker was not observed: {marker}"
                    )
        print("RA4_DIRECT_SAMBA_UPLOAD_AND_RUNTIME_PASS")
        return 0
    finally:
        if samba_may_be_active and not runtime_requested and bridge is not None and bridge.is_open:
            try:
                write_all(bridge, b"K#")
                print("SAMBA_FAILURE_RECOVERY=K#")
            except serial.SerialException:
                print("SAMBA_FAILURE_RECOVERY=K#_SEND_FAILED", file=sys.stderr)
        if bridge is not None and bridge.is_open:
            bridge.close()
        if not runtime_requested:
            try:
                restore_user_uart_baud(
                    args.bridge_port,
                    args.read_timeout_ms / 1000,
                    args.runtime_settle_ms / 1000,
                )
            except serial.SerialException:
                print("CDC_FAILURE_RECOVERY=FAILED", file=sys.stderr)
        relay.rts = False
        relay.close()
        print("Relay state: RA4M1 UART runtime route retained (RTS# HIGH, relay released).")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, serial.SerialException) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
