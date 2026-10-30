#!/usr/bin/env python3
"""Verify the native USB interfaces exposed by the BL616CL USB smoke image."""

from __future__ import annotations

import argparse
import array
import fcntl
import os
from pathlib import Path
import sys
import time

try:
    import serial
except ImportError as exc:
    print(f"pyserial is required: {exc}", file=sys.stderr)
    raise SystemExit(2)


EXPECTED_VENDOR = "2341"
EXPECTED_PRODUCT = "1002"
EXPECTED_SERIAL = "BL616CL-USB-SMOKE"
CDC_PAYLOAD = b"BL616CL_USB_SMOKE:CDC_ECHO\r\n"
HID_PAYLOAD = bytes([0x5A]) + bytes(range(1, 64))
FEATURE_PAYLOAD = bytes([0xA5]) + b"BL616CL-USB-FEATURE" + bytes(64 - 20)


def read_attribute(path: Path, name: str) -> str | None:
    try:
        return (path / name).read_text(encoding="ascii").strip()
    except OSError:
        return None


def usb_device_candidates() -> list[Path]:
    devices: list[Path] = []
    for entry in Path("/sys/bus/usb/devices").iterdir():
        if read_attribute(entry, "idVendor") != EXPECTED_VENDOR:
            continue
        if read_attribute(entry, "idProduct") != EXPECTED_PRODUCT:
            continue
        if read_attribute(entry, "serial") != EXPECTED_SERIAL:
            continue
        devices.append(entry)
    return devices


def wait_for_device(timeout_seconds: float) -> Path:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        candidates = usb_device_candidates()
        if len(candidates) == 1:
            return candidates[0]
        if len(candidates) > 1:
            raise RuntimeError(f"multiple BL616CL USB smoke devices found: {candidates}")
        time.sleep(0.1)
    raise RuntimeError(
        "native BL616CL USB smoke device did not enumerate; "
        "connect the DK GPIO32/GPIO33 USB path separately from CKLink-Lite"
    )


def interface_paths(device: Path) -> list[Path]:
    prefix = f"{device.name}:"
    return sorted(
        entry
        for entry in Path("/sys/bus/usb/devices").iterdir()
        if entry.name.startswith(prefix)
    )


def require_interface_classes(device: Path) -> list[Path]:
    interfaces = interface_paths(device)
    classes = {read_attribute(interface, "bInterfaceClass") for interface in interfaces}
    if "02" not in classes:
        raise RuntimeError(f"CDC control interface missing; observed classes: {sorted(classes)}")
    if "0a" not in classes:
        raise RuntimeError(f"CDC data interface missing; observed classes: {sorted(classes)}")
    if "03" not in classes:
        raise RuntimeError(f"HID interface missing; observed classes: {sorted(classes)}")
    return interfaces


def child_node(interface: Path, prefix: str) -> Path | None:
    for candidate in interface.rglob(f"{prefix}*"):
        if candidate.name.startswith(prefix) and candidate.name != prefix:
            return candidate
    return None


def require_cdc_node(interfaces: list[Path]) -> str:
    for interface in interfaces:
        tty = child_node(interface, "ttyACM")
        if tty is not None:
            return f"/dev/{tty.name}"
    raise RuntimeError("CDC interface did not create a ttyACM node")


def require_hidraw_node(interfaces: list[Path]) -> str:
    for interface in interfaces:
        hidraw = child_node(interface, "hidraw")
        if hidraw is not None:
            return f"/dev/{hidraw.name}"
    raise RuntimeError("HID interface did not create a hidraw node")


def verify_cdc_loopback(port: str) -> None:
    with serial.Serial(port, 115200, timeout=0.1, write_timeout=2) as link:
        link.reset_input_buffer()
        link.write(CDC_PAYLOAD)
        link.flush()
        deadline = time.monotonic() + 3.0
        received = bytearray()
        while time.monotonic() < deadline:
            waiting = link.in_waiting
            if waiting:
                received.extend(link.read(waiting))
                if CDC_PAYLOAD in received:
                    return
            else:
                time.sleep(0.01)
    raise RuntimeError(f"CDC loopback failed on {port}")


def read_hidraw(fd: int, expected: bytes, timeout_seconds: float) -> None:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        try:
            chunk = os.read(fd, 64)
        except BlockingIOError:
            time.sleep(0.01)
            continue
        if chunk:
            received.extend(chunk)
            if expected in received:
                return
    raise RuntimeError("HID output-to-input echo failed")


def hid_ioc(direction: int, number: int, length: int) -> int:
    ioc_nr_bits = 8
    ioc_type_bits = 8
    ioc_size_bits = 14
    ioc_nr_shift = 0
    ioc_type_shift = ioc_nr_shift + ioc_nr_bits
    ioc_size_shift = ioc_type_shift + ioc_type_bits
    ioc_dir_shift = ioc_size_shift + ioc_size_bits
    return (
        (direction << ioc_dir_shift)
        | (ord("H") << ioc_type_shift)
        | (number << ioc_nr_shift)
        | (length << ioc_size_shift)
    )


def verify_hid(hidraw: str, require_feature_io: bool) -> None:
    fd = os.open(hidraw, os.O_RDWR | os.O_NONBLOCK)
    try:
        written = os.write(fd, HID_PAYLOAD)
        if written != len(HID_PAYLOAD):
            raise RuntimeError(f"short HID write: {written} of {len(HID_PAYLOAD)} bytes")
        read_hidraw(fd, HID_PAYLOAD, 3.0)

        # hidraw feature ioctls include a leading report-number byte. This
        # descriptor has no report IDs, so that byte is zero.
        feature = array.array("B", [0]) + array.array("B", FEATURE_PAYLOAD)
        set_feature = hid_ioc(3, 0x06, len(feature))
        get_feature = hid_ioc(3, 0x07, len(feature))
        try:
            fcntl.ioctl(fd, set_feature, feature, True)
            response = array.array("B", [0]) + array.array("B", bytes(64))
            fcntl.ioctl(fd, get_feature, response, True)
            if bytes(response[1:]) != FEATURE_PAYLOAD:
                raise RuntimeError("HID feature report did not round-trip")
            print("HID feature report: PASS")
        except OSError as error:
            if require_feature_io:
                raise RuntimeError(f"HID feature ioctl unavailable: {error}") from error
            print(f"HID feature report: SKIPPED ({error})")
    finally:
        os.close(fd)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--require-feature-io", action="store_true")
    args = parser.parse_args()

    device = wait_for_device(args.timeout)
    interfaces = require_interface_classes(device)
    cdc_port = require_cdc_node(interfaces)
    hidraw = require_hidraw_node(interfaces)

    print(f"USB device: {device.name} {EXPECTED_VENDOR}:{EXPECTED_PRODUCT} {EXPECTED_SERIAL}")
    print(f"CDC node: {cdc_port}")
    print(f"HID node: {hidraw}")
    verify_cdc_loopback(cdc_port)
    print("CDC loopback: PASS")
    verify_hid(hidraw, args.require_feature_io)
    print("USB CDC + HID feasibility: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
