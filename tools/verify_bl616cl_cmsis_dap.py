#!/usr/bin/env python3
"""Read the RA4M1 SWD DP IDCODE through the BL616CL CMSIS-DAP HID interface."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import select
import sys
import time


EXPECTED_VENDOR = "2341"
EXPECTED_PRODUCT = "1002"
EXPECTED_SERIAL = "BL616CL-PROVISIONAL"
PACKET_SIZE = 64

CMD_DAP_INFO = 0x00
CMD_DAP_CONNECT = 0x02
CMD_DAP_DISCONNECT = 0x03
CMD_DAP_TRANSFER_CONFIGURE = 0x04
CMD_DAP_TRANSFER = 0x05
CMD_DAP_SWJ_CLOCK = 0x11
CMD_DAP_SWJ_SEQUENCE = 0x12
CMD_DAP_SWD_CONFIGURE = 0x13

DAP_PORT_SWD = 0x01
DAP_TRANSFER_OK = 0x01
DAP_TRANSFER_DP_READ_IDCODE = 0x02


def read_attribute(path: Path, name: str) -> str | None:
    try:
        return (path / name).read_text(encoding="ascii").strip()
    except OSError:
        return None


def find_usb_device() -> Path:
    matches: list[Path] = []
    for entry in Path("/sys/bus/usb/devices").iterdir():
        if read_attribute(entry, "idVendor") != EXPECTED_VENDOR:
            continue
        if read_attribute(entry, "idProduct") != EXPECTED_PRODUCT:
            continue
        if read_attribute(entry, "serial") != EXPECTED_SERIAL:
            continue
        matches.append(entry)

    if len(matches) != 1:
        raise RuntimeError(
            "expected exactly one BL616CL provisional CMSIS-DAP USB device, "
            f"found {len(matches)}"
        )
    return matches[0]


def find_hidraw(device: Path) -> str:
    prefix = f"{device.name}:"
    hid_interfaces = {
        interface.resolve()
        for interface in Path("/sys/bus/usb/devices").iterdir()
        if interface.name.startswith(prefix)
        and read_attribute(interface, "bInterfaceClass") == "03"
    }
    for hidraw in sorted(Path("/sys/class/hidraw").glob("hidraw*")):
        # Under WSL USB/IP, hidraw lives below the HID device directory instead
        # of the USB-interface symlink, so rglob() from the latter cannot see it.
        hid_device = (hidraw / "device").resolve()
        if hid_device.parent in hid_interfaces:
            return f"/dev/{hidraw.name}"

    raise RuntimeError("CMSIS-DAP HID interface did not create a hidraw node")


def read_packet(fd: int, timeout_seconds: float) -> bytes:
    ready, _, _ = select.select([fd], [], [], timeout_seconds)
    if not ready:
        raise RuntimeError("timed out waiting for CMSIS-DAP response")

    response = os.read(fd, PACKET_SIZE)
    if len(response) != PACKET_SIZE:
        raise RuntimeError(
            f"short CMSIS-DAP response: {len(response)} of {PACKET_SIZE} bytes"
        )
    return response


def transact(fd: int, request: bytes) -> bytes:
    # CherryUSB's HID endpoint consumes the first byte as the report-ID
    # placeholder even though the report descriptor does not declare an
    # explicit report ID. The Linux hidraw node therefore needs a leading
    # zero on writes, while reads expose the 64-byte CMSIS-DAP payload.
    if len(request) > PACKET_SIZE - 1:
        raise ValueError("CMSIS-DAP request exceeds 64 bytes")

    packet = (b"\0" + request).ljust(PACKET_SIZE, b"\0")
    written = os.write(fd, packet)
    if written != len(packet):
        raise RuntimeError(
            f"short CMSIS-DAP write: {written} of {len(packet)} bytes"
        )

    response = read_packet(fd, 3.0)
    if response[0] != request[0]:
        raise RuntimeError(
            f"unexpected CMSIS-DAP response command 0x{response[0]:02x}, "
            f"expected 0x{request[0]:02x}"
        )
    return response


def require_ok(response: bytes, command_name: str) -> None:
    if response[1] != 0:
        raise RuntimeError(f"{command_name} failed with status 0x{response[1]:02x}")


def dap_info(fd: int, info_id: int) -> bytes:
    response = transact(fd, bytes((CMD_DAP_INFO, info_id)))
    length = response[1]
    if length > PACKET_SIZE - 2:
        raise RuntimeError(f"DAP_Info returned invalid length {length}")
    return response[2 : 2 + length]


def dap_swj_sequence(fd: int, bit_count: int, data: bytes) -> None:
    response = transact(fd, bytes((CMD_DAP_SWJ_SEQUENCE, bit_count)) + data)
    require_ok(response, "DAP_SWJ_Sequence")


def probe_dp_idcode(fd: int) -> int:
    vendor = dap_info(fd, 0x01).rstrip(b"\0").decode("ascii", errors="replace")
    product = dap_info(fd, 0x02).rstrip(b"\0").decode("ascii", errors="replace")
    packet_size = dap_info(fd, 0xFF)
    capabilities = dap_info(fd, 0xF0)
    print(f"CMSIS-DAP: vendor={vendor!r} product={product!r}")
    print(
        "CMSIS-DAP: "
        f"packet_size={int.from_bytes(packet_size, 'little')} "
        f"capabilities=0x{capabilities[0] if capabilities else 0:02x}"
    )

    connected = False
    try:
        response = transact(fd, bytes((CMD_DAP_CONNECT, DAP_PORT_SWD)))
        if response[1] != DAP_PORT_SWD:
            raise RuntimeError(f"DAP_Connect did not select SWD: 0x{response[1]:02x}")
        connected = True

        response = transact(fd, bytes((CMD_DAP_SWJ_CLOCK,)) + (100_000).to_bytes(4, "little"))
        require_ok(response, "DAP_SWJ_Clock")

        response = transact(fd, bytes((CMD_DAP_SWD_CONFIGURE, 0x00)))
        require_ok(response, "DAP_SWD_Configure")

        response = transact(
            fd,
            bytes((CMD_DAP_TRANSFER_CONFIGURE, 0x00, 100, 0x00, 100, 0x00)),
        )
        require_ok(response, "DAP_TransferConfigure")

        # SWD line reset, JTAG-to-SWD select (LSB first), then line reset.
        dap_swj_sequence(fd, 56, b"\xff" * 7)
        dap_swj_sequence(fd, 16, bytes((0x9E, 0xE7)))
        dap_swj_sequence(fd, 56, b"\xff" * 7)

        response = transact(
            fd,
            bytes((CMD_DAP_TRANSFER, 0x00, 0x01, DAP_TRANSFER_DP_READ_IDCODE)),
        )
        if response[1] != 1 or response[2] != DAP_TRANSFER_OK:
            raise RuntimeError(
                "DP IDCODE read failed: "
                f"count={response[1]} ack=0x{response[2]:02x}"
            )

        idcode = int.from_bytes(response[3:7], "little")
        if idcode in (0x00000000, 0xFFFFFFFF):
            raise RuntimeError(f"invalid DP IDCODE 0x{idcode:08x}")
        return idcode
    finally:
        if connected:
            response = transact(fd, bytes((CMD_DAP_DISCONNECT,)))
            require_ok(response, "DAP_Disconnect")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--hidraw",
        help="CMSIS-DAP hidraw node; omit to discover BL616CL-PROVISIONAL",
    )
    args = parser.parse_args()

    hidraw = args.hidraw
    if hidraw is None:
        device = find_usb_device()
        hidraw = find_hidraw(device)
        print(f"USB device: {device.name} {EXPECTED_VENDOR}:{EXPECTED_PRODUCT} {EXPECTED_SERIAL}")
    if not Path(hidraw).is_char_device():
        raise RuntimeError(f"missing HID device: {hidraw}")

    fd = os.open(hidraw, os.O_RDWR | os.O_NONBLOCK)
    try:
        idcode = probe_dp_idcode(fd)
    finally:
        os.close(fd)

    print(f"RA4M1 SWD DP IDCODE: 0x{idcode:08x}")
    print("CMSIS-DAP SWD read-only probe: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
