#!/usr/bin/env python3
"""Resolve UNO R4 test devices from their USB identity instead of tty order."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path


PAIRED_BL616CL_SERIAL = "BL616CL-PROVISIONAL"
BASELINE_ESP32_SERIALS = frozenset(s.strip().upper() for s in os.environ.get('UNO_R4_BASELINE_SERIALS',os.environ.get('UNO_R4_BASELINE_SERIAL','')).split(',') if s.strip())


def tty_properties(port: str) -> dict[str, str]:
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
    return dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)


def find_unique_tty(
    *,
    vendor: str,
    product: str,
    interface: str,
    serial: str | None = None,
    serials: frozenset[str] | None = None,
) -> str:
    if serial is not None and serials is not None:
        raise RuntimeError("serial and serials cannot be requested together")

    matches: list[str] = []
    for tty_path in sorted(Path("/sys/class/tty").glob("tty*")):
        port = f"/dev/{tty_path.name}"
        if not Path(port).exists():
            continue
        try:
            properties = tty_properties(port)
        except RuntimeError:
            continue
        if (
            properties.get("ID_VENDOR_ID", "").lower() != vendor.lower()
            or properties.get("ID_MODEL_ID", "").lower() != product.lower()
            or properties.get("ID_USB_INTERFACE_NUM", "").lower() != interface.lower()
        ):
            continue
        if serial is not None and properties.get("ID_SERIAL_SHORT") != serial:
            continue
        if (
            serials is not None
            and properties.get("ID_SERIAL_SHORT") not in serials
        ):
            continue
        matches.append(port)

    if len(matches) != 1:
        description = f"{vendor}:{product} interface {interface}"
        if serial is not None:
            description += f" serial {serial}"
        if serials is not None:
            description += " serial in {" + ", ".join(sorted(serials)) + "}"
        found = ", ".join(matches) if matches else "none"
        raise RuntimeError(f"expected exactly one {description} USB tty, found: {found}")
    return matches[0]


def resolve_paired_bl616cl_port(port: str) -> str:
    if port != "auto":
        return port
    return find_unique_tty(
        vendor="2341",
        product="1002",
        interface="00",
        serial=PAIRED_BL616CL_SERIAL,
    )


def resolve_baseline_esp32_port(port: str) -> str:
    if port != "auto":
        return port
    return find_unique_tty(
        vendor="2341",
        product="1002",
        interface="01",
        serials=BASELINE_ESP32_SERIALS,
    )


def resolve_relay_port(port: str) -> str:
    if port != "auto":
        return port
    return find_unique_tty(vendor="0403", product="6010", interface="00")
