#!/usr/bin/env python3
"""Upload the paired-board RA4M1 heartbeat image through split SAM-BA UARTs.

The BL616CL USB CDC/GPIO34 path is TX-only for the hand-wired board. RA4M1
responses are captured through FTDI interface 00, so conventional single-port
SAM-BA/BOSSA tools cannot be used for this gate.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
import time
from pathlib import Path

import serial


# Arduino's SAM-BA extended bootloader exposes application Flash from logical
# address zero. The RA4M1 CPU maps the application's vector table at 0x4000,
# but BOSSA's writeBuffer()/erase commands must use this bootloader address.
APPLICATION_FLASH_ADDRESS = 0x00000000
FLASH_END_ADDRESS = 0x00040000
FLASH_PAGE_SIZE = 256
WRITE_BUFFER_SIZE = 4096
WRITE_BUFFER_ADDRESS = 0x00000034
WORD_COPY_STACK_ADDRESS = 0x00000020
WORD_COPY_WORDS_ADDRESS = 0x00000030

# The same compatibility applet used by BossaUnoR4WiFi and the Windows runner.
WORD_COPY_APPLET = bytes(
    (
        0x09, 0x48, 0x0A, 0x49, 0x0A, 0x4A, 0x02, 0xE0,
        0x08, 0xC9, 0x08, 0xC0, 0x01, 0x3A, 0x00, 0x2A,
        0xFA, 0xD1, 0x04, 0x48, 0x00, 0x28, 0x01, 0xD1,
        0x01, 0x48, 0x85, 0x46, 0x70, 0x47, 0xC0, 0x46,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    )
)


def hex_bytes(value: bytes) -> str:
    return value.hex("-") if value else "<none>"


def open_serial(port: str, baudrate: int, timeout: float) -> serial.Serial:
    link = serial.Serial()
    link.port = port
    link.baudrate = baudrate
    link.timeout = timeout
    link.write_timeout = timeout
    link.dtr = False
    link.rts = False
    link.open()
    link.dtr = False
    link.rts = False
    return link


def read_exact(link: serial.Serial, count: int, timeout: float, description: str) -> bytes:
    deadline = time.monotonic() + timeout
    received = bytearray()
    while len(received) < count and time.monotonic() < deadline:
        chunk = link.read(count - len(received))
        if chunk:
            received.extend(chunk)
    if len(received) != count:
        raise RuntimeError(
            f"{description} timed out after receiving {len(received)}/{count} byte(s)"
        )
    return bytes(received)


def capture(link: serial.Serial, duration: float, maximum: int) -> bytes:
    deadline = time.monotonic() + duration
    received = bytearray()
    while time.monotonic() < deadline:
        chunk = link.read(256)
        if chunk:
            received.extend(chunk)
            if len(received) > maximum:
                raise RuntimeError(f"serial capture exceeded {maximum} byte(s)")
    return bytes(received)


def write_all(link: serial.Serial, payload: bytes) -> None:
    if payload:
        link.write(payload)
        link.flush()


def write_paced(
    link: serial.Serial, payload: bytes, packet_bytes: int, delay_seconds: float
) -> None:
    for offset in range(0, len(payload), packet_bytes):
        write_all(link, payload[offset : offset + packet_bytes])
        if offset + packet_bytes < len(payload):
            time.sleep(delay_seconds)


def command(command_name: str, address: int, size: int) -> bytes:
    return f"{command_name}{address:08X},{size:08X}#".encode("ascii")


def samba_checksum(data: bytes) -> int:
    checksum = 0
    for value in data:
        checksum ^= value << 8
        for _ in range(8):
            if checksum & 0x8000:
                checksum = ((checksum << 1) ^ 0x1021) & 0xFFFF
            else:
                checksum = (checksum << 1) & 0xFFFF
    return checksum


def expect_response(
    bridge: serial.Serial,
    reference: serial.Serial,
    payload: bytes,
    expected: int,
    timeout: float,
    description: str,
) -> None:
    reference.reset_input_buffer()
    write_all(bridge, payload)
    reply = read_exact(reference, 3, timeout, description)
    if reply != bytes((expected, 0x0A, 0x0D)):
        raise RuntimeError(f"{description} returned unexpected reply {hex_bytes(reply)}")


def send_samba_write(
    bridge: serial.Serial,
    reference: serial.Serial,
    address: int,
    payload: bytes,
    packet_bytes: int,
    delay_seconds: float,
    description: str,
) -> None:
    reference.reset_input_buffer()
    write_all(bridge, command("S", address, len(payload)))
    time.sleep(0.002)
    write_paced(bridge, payload, packet_bytes, delay_seconds)
    packet_count = (len(payload) + packet_bytes - 1) // packet_bytes
    print(
        f"{description} direct_payload_bytes={len(payload)} packets={packet_count} "
        f"packet_bytes={packet_bytes} delay_ms={int(delay_seconds * 1000)}"
    )


def send_samba_word(
    bridge: serial.Serial,
    address: int,
    value: int,
    description: str,
) -> None:
    """Configure the RAM word-copy applet using SAM-BA's no-reply W command."""
    write_all(bridge, f"W{address:08X},{value:08X}#".encode("ascii"))
    time.sleep(0.01)
    print(f"{description} address=0x{address:08X} value=0x{value:08X}")


def verify_flash(
    bridge: serial.Serial,
    reference: serial.Serial,
    image: bytes,
    timeout: float,
) -> None:
    for chunk_index, offset in enumerate(range(0, len(image), WRITE_BUFFER_SIZE)):
        payload = image[offset : offset + WRITE_BUFFER_SIZE]
        flash_address = APPLICATION_FLASH_ADDRESS + offset
        reference.reset_input_buffer()
        write_all(bridge, command("Z", flash_address, len(payload)))
        reply = read_exact(
            reference,
            12,
            timeout,
            f"Flash chunk {chunk_index} checksum 0x{flash_address:08X}",
        )
        if reply[0:1] != b"Z" or reply[9:] != b"#\n\r":
            raise RuntimeError(
                f"Flash chunk {chunk_index} checksum returned unexpected reply "
                f"{hex_bytes(reply)}"
            )
        try:
            target_checksum = int(reply[1:9].decode("ascii"), 16)
        except ValueError as error:
            raise RuntimeError(
                f"Flash chunk {chunk_index} checksum was not hexadecimal: {hex_bytes(reply)}"
            ) from error
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


def initialize_samba(bridge: serial.Serial, reference: serial.Serial, timeout: float) -> str:
    time.sleep(0.1)
    for index in range(1, 3):
        reference.reset_input_buffer()
        write_all(bridge, b"\x80")
        reply = capture(reference, timeout, 16)
        print(f"SAMBA_AUTO_BAUD_{index}={hex_bytes(reply)}")

    reference.reset_input_buffer()
    write_all(bridge, b"#")
    reply = capture(reference, timeout, 16)
    print(f"SAMBA_AUTO_BAUD_CONFIRM={hex_bytes(reply)}")

    reference.reset_input_buffer()
    write_all(bridge, b"N#")
    reply = read_exact(reference, 2, timeout, "SAM-BA binary mode")
    if reply != b"\n\r":
        raise RuntimeError(f"SAM-BA binary mode returned unexpected reply {hex_bytes(reply)}")
    print(f"SAMBA_BINARY_MODE={hex_bytes(reply)}")

    reference.reset_input_buffer()
    write_all(bridge, b"V#")
    version = capture(reference, timeout, 128).decode("ascii", errors="replace").strip()
    if not version.startswith("Arduino Bootloader (SAM-BA extended) "):
        raise RuntimeError(f"unexpected SAM-BA version reply: {version!r}")
    print(f"SAMBA_VERSION={version}")
    return version


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Upload and validate the RA4M1 heartbeat Sketch through split SAM-BA UARTs."
    )
    parser.add_argument(
        "--image",
        type=Path,
        default=Path("build/ra4-uart-heartbeat/ra4-uart-heartbeat.ino.bin"),
    )
    parser.add_argument("--bridge-port", default="/dev/ttyACM1")
    parser.add_argument("--reference-port", default="/dev/ttyUSB0")
    parser.add_argument("--bootstrap-settle-ms", type=int, default=500)
    parser.add_argument("--runtime-settle-ms", type=int, default=300)
    parser.add_argument("--read-timeout-ms", type=int, default=1000)
    parser.add_argument("--long-read-timeout-ms", type=int, default=5000)
    parser.add_argument("--runtime-capture-ms", type=int, default=3500)
    parser.add_argument("--payload-packet-bytes", type=int, default=64)
    parser.add_argument("--payload-interpacket-delay-ms", type=int, default=5)
    parser.add_argument(
        "--verify-crc16",
        action="store_true",
        help="Request the bootloader Z checksum for each image chunk after programming.",
    )
    parser.add_argument(
        "--verify-only",
        action="store_true",
        help="Request Z checksums for the existing heartbeat image without programming Flash.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if (
        args.bootstrap_settle_ms <= 0
        or args.runtime_settle_ms <= 0
        or args.read_timeout_ms <= 0
        or args.long_read_timeout_ms <= 0
        or args.runtime_capture_ms < 2000
        or args.payload_packet_bytes <= 0
        or args.payload_interpacket_delay_ms < 0
    ):
        raise RuntimeError("invalid timeout or packet-size argument")

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

    print(f"Bridge:    {args.bridge_port}")
    print(f"Reference: {args.reference_port}")
    print(f"Image:     {image_path}")
    print(f"Image size: {len(image)}")
    print(f"Image SHA256: {hashlib.sha256(image).hexdigest()}")
    if args.verify_only:
        print("Target: RA4M1 application Flash only. Verify-only mode does not erase or program Flash.")
    else:
        print("Target: RA4M1 application Flash only. Upload erases and replaces the current RA4M1 Sketch.")
    print("Relay route: RA4M1 UART (RTS=False, physical RTS# HIGH, relay released).")

    reference = open_serial(args.reference_port, 230400, args.read_timeout_ms / 1000)
    bridge: serial.Serial | None = None
    samba_active = False
    runtime_requested = False
    try:
        print(f"RELAY_RUNTIME_ROUTE rts={reference.rts}")
        time.sleep(0.05)
        bridge = open_serial(args.bridge_port, 115200, args.read_timeout_ms / 1000)
        bridge.baudrate = 1200
        print("CDC_SET_LINE_CODING=1200")
        time.sleep(args.bootstrap_settle_ms / 1000)
        bridge.close()

        bridge = open_serial(args.bridge_port, 230400, args.read_timeout_ms / 1000)
        initialize_samba(bridge, reference, args.read_timeout_ms / 1000)
        samba_active = True

        if not args.verify_only:
            delay_seconds = args.payload_interpacket_delay_ms / 1000
            send_samba_write(
                bridge,
                reference,
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
                reference,
                f"X{APPLICATION_FLASH_ADDRESS:08X}#".encode("ascii"),
                ord("X"),
                args.long_read_timeout_ms / 1000,
                "RA4M1 application Flash erase",
            )
            print(f"RA4_FLASH_ERASED_FROM=0x{APPLICATION_FLASH_ADDRESS:08X}")

            flash_address = APPLICATION_FLASH_ADDRESS
            chunk_count = 0
            for offset in range(0, len(image), WRITE_BUFFER_SIZE):
                payload = image[offset : offset + WRITE_BUFFER_SIZE]
                program_size = (len(payload) + FLASH_PAGE_SIZE - 1) // FLASH_PAGE_SIZE * FLASH_PAGE_SIZE
                staged = payload.ljust(program_size, b"\x00")

                send_samba_write(
                    bridge,
                    reference,
                    WRITE_BUFFER_ADDRESS,
                    staged,
                    args.payload_packet_bytes,
                    delay_seconds,
                    f"Flash chunk {chunk_count} staging",
                )
                expect_response(
                    bridge,
                    reference,
                    f"Y{WRITE_BUFFER_ADDRESS:08X},0#".encode("ascii"),
                    ord("Y"),
                    args.read_timeout_ms / 1000,
                    f"Flash chunk {chunk_count} select staging buffer",
                )
                expect_response(
                    bridge,
                    reference,
                    command("Y", flash_address, program_size),
                    ord("Y"),
                    args.long_read_timeout_ms / 1000,
                    f"Flash chunk {chunk_count} write 0x{flash_address:08X}",
                )
                print(
                    f"RA4_FLASH_CHUNK_PASS index={chunk_count} address=0x{flash_address:08X} "
                    f"programmed={program_size} payload={len(payload)}"
                )
                flash_address += program_size
                chunk_count += 1

            print(f"RA4_FLASH_WRITE_COMMAND_PASS chunks={chunk_count}")

        if args.verify_crc16 or args.verify_only:
            verify_flash(bridge, reference, image, args.long_read_timeout_ms / 1000)
        reference.reset_input_buffer()
        write_all(bridge, b"K#")
        runtime_requested = True
        print("SAMBA_RESET_COMMAND=K#")
        time.sleep(0.05)
        reference.baudrate = 115200
        bridge.baudrate = 115200
        time.sleep(args.runtime_settle_ms / 1000)
        print("RA4 runtime requested by K#; user UART restored directly to 115200.")

        runtime = capture(reference, args.runtime_capture_ms / 1000, 8192)
        runtime_text = runtime.decode("ascii", errors="replace")
        print("RA4_RUNTIME_CAPTURE_BEGIN")
        print(runtime_text, end="" if runtime_text.endswith("\n") else "\n")
        print("RA4_RUNTIME_CAPTURE_END")
        if "RA4_HEARTBEAT:BOOT" not in runtime_text:
            raise RuntimeError("RA4M1 heartbeat boot marker was not observed")
        heartbeat_count = sum(
            1
            for line in runtime_text.splitlines()
            if line.startswith("RA4_HEARTBEAT:") and line != "RA4_HEARTBEAT:BOOT"
        )
        if heartbeat_count < 2:
            raise RuntimeError("fewer than two RA4M1 heartbeat markers were observed")

        nonce = f"bl616cl-split-{time.monotonic_ns():x}"
        reference.reset_input_buffer()
        write_all(bridge, f"{nonce}\n".encode("ascii"))
        echo_text = capture(reference, args.read_timeout_ms / 1000, 2048).decode(
            "ascii", errors="replace"
        )
        print(f"RA4_ECHO_CAPTURE={echo_text}")
        if f"RA4_ECHO:{nonce}" not in echo_text:
            raise RuntimeError(f"RA4M1 nonce echo was not observed: expected RA4_ECHO:{nonce}")
        print(f"RA4_SPLIT_SAMBA_UPLOAD_AND_RUNTIME_PASS nonce={nonce}")
        return 0
    finally:
        if samba_active and not runtime_requested and bridge is not None and bridge.is_open:
            try:
                write_all(bridge, b"K#")
                print("SAMBA_FAILURE_RECOVERY=K#")
            except serial.SerialException:
                print("SAMBA_FAILURE_RECOVERY=K#_SEND_FAILED", file=sys.stderr)
        if bridge is not None and bridge.is_open:
            bridge.close()
        reference.rts = False
        reference.close()
        print("Relay state: RA4M1 UART runtime route retained (RTS# HIGH, relay released).")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
