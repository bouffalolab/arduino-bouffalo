#!/usr/bin/env python3
"""Program and verify an RA4M1 application image through BL616CL SWD."""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import serial

from uno_r4_usb_identity import resolve_paired_bl616cl_port, tty_properties


ROOT = Path(__file__).resolve().parents[1]
LOADER_DIR = ROOT / "build/ra4-swd-flash-loader"
LOADER_BIN = LOADER_DIR / "ra4_swd_flash_loader.bin"
DEFAULT_IMAGE = ROOT / "build/ra4-swd-sram-scratch/ra4-swd-sram-scratch.ino.bin"
DEFAULT_RESTORE = ROOT / "tools/run_ra4_wifis3_scan_final_carrier.py"

LOADER_ADDR = 0x20004000
MAILBOX_ADDR = 0x20006000
STAGING_ADDR = 0x20006800
STACK_TOP = 0x20007C00
LOADER_ENTRY = LOADER_ADDR | 1

APPLICATION_FLASH_START = 0x00004000
FLASH_END = 0x00040000
ERASE_BLOCK = 0x800
PROGRAM_UNIT = 8
MAILBOX_MAGIC = 0x4450374C
MAILBOX_VERSION = 1
COMMAND_ERASE = 1
COMMAND_PROGRAM = 2
COMMAND_CLEANUP = 3
STATUS_DONE = 3
STATUS_ERROR = 4
MAILBOX_WORDS = 11
SCRATCH_MARKER = b"RA4_SWD_SCRATCH:READY"


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--image", type=Path, default=DEFAULT_IMAGE)
    parser.add_argument("--loader-bin", type=Path, default=LOADER_BIN)
    parser.add_argument("--openocd", type=Path)
    parser.add_argument("--target-config", type=Path)
    parser.add_argument("--scripts-dir", type=Path)
    parser.add_argument("--adapter-khz", type=int, default=100)
    parser.add_argument("--erase-timeout-ms", type=int, default=1200)
    parser.add_argument("--program-timeout-ms", type=int, default=1200)
    parser.add_argument(
        "--stop-after-commands",
        type=int,
        help=(
            "stop after this many completed erase/program mailbox commands; "
            "used only for D8 controlled interruption recovery"
        ),
    )
    parser.add_argument("--skip-restore-scan", action="store_true")
    return parser.parse_args()


def require_final_carrier(port: str) -> None:
    properties = tty_properties(port)
    expected = {
        "ID_VENDOR_ID": "2341",
        "ID_MODEL_ID": "1002",
        "ID_USB_INTERFACE_NUM": "00",
        "ID_SERIAL_SHORT": "BL616CL-PROVISIONAL",
    }
    for key, value in expected.items():
        if properties.get(key, "").lower() != value.lower():
            raise RuntimeError(f"{port} is not the final-carrier BL616CL CDC interface")


def find_one(description: str, paths: list[Path]) -> Path:
    matches = [path for path in paths if path.is_file()]
    if len(matches) != 1:
        raise RuntimeError(
            f"expected exactly one {description}, found: "
            + (", ".join(map(str, matches)) or "none")
        )
    return matches[0].resolve()


def resolve_openocd(args: argparse.Namespace) -> tuple[Path, Path, Path]:
    roots = sorted(Path.home().glob(".arduino15/packages/arduino/tools/openocd/*"))
    openocd = args.openocd or find_one(
        "OpenOCD executable", [root / "bin/openocd" for root in roots]
    )
    scripts = args.scripts_dir or openocd.parents[1] / "share/openocd/scripts"
    target = args.target_config or find_one(
        "RA4M1 target config",
        sorted(
            Path.home().glob(
                ".arduino15/packages/arduino/hardware/renesas_uno/*/debugger/R7FA4M1AB.cfg"
            )
        ),
    )
    if not scripts.is_dir() or not target.is_file():
        raise RuntimeError("OpenOCD scripts or RA4M1 target config is unavailable")
    return openocd, scripts.resolve(), target.resolve()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def padded_image(path: Path) -> bytes:
    data = path.read_bytes()
    if not data or APPLICATION_FLASH_START + len(data) > FLASH_END:
        raise RuntimeError("image must be non-empty and fit RA4M1 code Flash")
    padded_size = (len(data) + ERASE_BLOCK - 1) & ~(ERASE_BLOCK - 1)
    return data + (b"\xFF" * (padded_size - len(data)))


def mailbox_words(command: int, address: int, length: int, source: int) -> list[int]:
    return [
        MAILBOX_MAGIC,
        MAILBOX_VERSION,
        command,
        1,
        address,
        length,
        source,
        0,
        0,
        0,
        0,
    ]


def mww_lines(address: int, words: list[int]) -> list[str]:
    return [f"mww 0x{address + index * 4:08X} 0x{value:08X}" for index, value in enumerate(words)]


def command_lines(command: int, address: int, length: int, source: int, timeout_ms: int) -> list[str]:
    return [
        f"mww 0x{MAILBOX_ADDR + 16:08X} 0x{address:08X}",
        f"mww 0x{MAILBOX_ADDR + 20:08X} 0x{length:08X}",
        f"mww 0x{MAILBOX_ADDR + 24:08X} 0x{source:08X}",
        f"mww 0x{MAILBOX_ADDR + 8:08X} 0x{command:08X}",
        "resume",
        f"sleep {timeout_ms}",
        "halt",
        f"mdw 0x{MAILBOX_ADDR:08X} {MAILBOX_WORDS}",
    ]


def openocd_command(args: argparse.Namespace, script: Path) -> list[str]:
    openocd, scripts, target = resolve_openocd(args)
    return [
        str(openocd),
        "-s",
        str(scripts),
        "-f",
        "interface/cmsis-dap.cfg",
        "-f",
        str(target),
        "-c",
        f"adapter speed {args.adapter_khz}",
        "-c",
        "cmsis_dap_serial BL616CL-PROVISIONAL",
        "-f",
        str(script),
    ]


def parse_mailbox_snapshots(output: str) -> list[list[int]]:
    lines = output.splitlines()
    snapshots: list[list[int]] = []
    first_line = re.compile(rf"(?i)^0x{MAILBOX_ADDR:08x}:\s+(.+)$")
    continuation = re.compile(r"(?i)^0x([0-9a-f]{8}):\s+(.+)$")
    for index, line in enumerate(lines):
        first = first_line.match(line.strip())
        if first is None:
            continue
        words = [int(value, 16) for value in first.group(1).split()]
        expected_address = MAILBOX_ADDR + len(words) * 4
        cursor = index + 1
        while len(words) < MAILBOX_WORDS and cursor < len(lines):
            match = continuation.match(lines[cursor].strip())
            if match is None or int(match.group(1), 16) != expected_address:
                break
            values = [int(value, 16) for value in match.group(2).split()]
            words.extend(values)
            expected_address += len(values) * 4
            cursor += 1
        if len(words) >= MAILBOX_WORDS:
            snapshots.append(words[:MAILBOX_WORDS])
    return snapshots


def create_openocd_script(
    loader: bytes,
    image: bytes,
    temp_dir: Path,
    erase_count: int,
    erase_timeout_ms: int,
    program_timeout_ms: int,
    dump_path: Path,
    stop_after_commands: int | None,
) -> str:
    loader_path = temp_dir / "loader.bin"
    loader_path.write_bytes(loader)

    lines = [
        "proc d7_mailbox_snapshot {sequence} {",
        f'    set dump [capture "mdw 0x{MAILBOX_ADDR:08X} {MAILBOX_WORDS}"]',
        '    echo "D7_MAILBOX_BEGIN $sequence"',
        "    echo $dump",
        '    echo "D7_MAILBOX_END $sequence"',
        f"    mem2array mailbox 32 0x{MAILBOX_ADDR:08X} {MAILBOX_WORDS}",
        "    if {$sequence == 0} {",
        '        if {$mailbox(3) != 1} {error "loader did not become ready"}',
        "    } else {",
        '        if {$mailbox(2) != 0 || $mailbox(3) != 3 || $mailbox(10) != 0} {error "loader command failed"}',
        "    }",
        "}",
        "init",
        # Start from reset state: an arbitrary halted Sketch/bootloader can
        # retain NMI/peripheral state that PRIMASK cannot disable.
        "reset halt",
        f"load_image {loader_path} 0x{LOADER_ADDR:08X} bin",
        *mww_lines(
            MAILBOX_ADDR,
            [
                MAILBOX_MAGIC,
                MAILBOX_VERSION,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
            ],
        ),
        f"reg r0 0x{MAILBOX_ADDR:08X}",
        "reg control 0",
        "reg xpsr 0x01000000",
        f"reg sp 0x{STACK_TOP:08X}",
        f"reg pc 0x{LOADER_ENTRY:08X}",
        "resume",
        "sleep 20",
        "halt",
        "d7_mailbox_snapshot 0",
    ]

    # Each command runs to completion before one SWD halt collects its mailbox.
    # The loader operates wholly in SRAM, so this needs neither a Flash breakpoint
    # nor an interrupt/vector handler during Code Flash P/E mode.
    snapshot_sequence = 1
    completed_commands = 0
    for block in range(erase_count):
        address = APPLICATION_FLASH_START + block * ERASE_BLOCK
        lines += command_lines(
            COMMAND_ERASE,
            address,
            ERASE_BLOCK,
            0,
            erase_timeout_ms,
        )
        lines[-1] = f"d7_mailbox_snapshot {snapshot_sequence}"
        snapshot_sequence += 1
        completed_commands += 1
        if completed_commands == stop_after_commands:
            lines += ["shutdown"]
            return "\n".join(lines) + "\n"
    for offset in range(0, len(image), 0x800):
        chunk = image[offset : offset + 0x800]
        chunk_path = temp_dir / f"chunk-{offset:08x}.bin"
        chunk_path.write_bytes(chunk)
        lines += [
            f"load_image {chunk_path} 0x{STAGING_ADDR:08X} bin",
        ]
        lines += command_lines(
            COMMAND_PROGRAM,
            APPLICATION_FLASH_START + offset,
            len(chunk),
            STAGING_ADDR,
            program_timeout_ms,
        )
        lines[-1] = f"d7_mailbox_snapshot {snapshot_sequence}"
        snapshot_sequence += 1
        completed_commands += 1
        if completed_commands == stop_after_commands:
            lines += ["shutdown"]
            return "\n".join(lines) + "\n"

    lines += [
        f"dump_image {dump_path} 0x{APPLICATION_FLASH_START:08X} 0x{len(image):08X}",
    ]
    lines += command_lines(
        COMMAND_CLEANUP,
        0,
        0,
        0,
        100,
    )
    lines[-1] = f"d7_mailbox_snapshot {snapshot_sequence}"
    lines += [
        "reset run",
        "shutdown",
    ]
    return "\n".join(lines) + "\n"


def capture_scratch(port: str, timeout: float = 8.0) -> str:
    deadline = time.monotonic() + timeout
    with serial.Serial(port, 115200, timeout=0.1, write_timeout=1.0) as bridge:
        bridge.dtr = False
        bridge.rts = False
        bridge.reset_input_buffer()
        captured = bytearray()
        while time.monotonic() < deadline:
            captured.extend(bridge.read(512))
            if SCRATCH_MARKER in captured:
                return captured.decode("ascii", errors="replace")
    raise RuntimeError("RA4M1 scratch Sketch did not produce its runtime marker")


def run() -> int:
    args = parse_arguments()
    if args.adapter_khz <= 0 or args.erase_timeout_ms <= 0 or args.program_timeout_ms <= 0:
        raise RuntimeError("timeouts and adapter speed must be positive")
    bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier(bridge_port)
    image_path = args.image.resolve()
    loader_path = args.loader_bin.resolve()
    if not image_path.is_file():
        raise RuntimeError(f"RA4M1 image does not exist: {image_path}")
    if not loader_path.is_file():
        raise RuntimeError(
            f"loader does not exist: {loader_path}; build it first with "
            "build_ra4_swd_flash_loader.py"
        )

    image = padded_image(image_path)
    erase_count = len(image) // ERASE_BLOCK
    program_count = len(image) // ERASE_BLOCK
    operation_count = erase_count + program_count
    if args.stop_after_commands is not None and not (
        1 <= args.stop_after_commands < operation_count
    ):
        raise RuntimeError(
            "--stop-after-commands must be between 1 and "
            f"{operation_count - 1}"
        )
    original_hash = sha256(image_path)
    print(f"BL616CL_FINAL_CARRIER_PORT={bridge_port}")
    print(f"RA4_SWD_D7_IMAGE={image_path}")
    print(f"RA4_SWD_D7_IMAGE_BYTES={len(image_path.read_bytes())}")
    print(f"RA4_SWD_D7_IMAGE_SHA256={original_hash}")
    print(
        f"RA4_SWD_D7_PROGRAM_RANGE=0x{APPLICATION_FLASH_START:08X}.."
        f"0x{APPLICATION_FLASH_START + len(image):08X}"
    )
    print(f"RA4_SWD_D7_ERASE_BLOCKS={erase_count}")
    print("Target: RA4M1 application Code Flash only; BL616CL Flash is untouched.")

    loader = loader_path.read_bytes()
    with tempfile.TemporaryDirectory(prefix="ra4-d7-") as temp_name:
        temp_dir = Path(temp_name)
        dump_path = temp_dir / "readback.bin"
        output_script = temp_dir / "session.cfg"
        output_script.write_text(
            create_openocd_script(
                loader,
                image,
                temp_dir,
                erase_count,
                args.erase_timeout_ms,
                args.program_timeout_ms,
                dump_path,
                args.stop_after_commands,
            ),
            encoding="ascii",
        )

        try:
            command = openocd_command(args, output_script)
            print("COMMAND=" + " ".join(command))
            result = subprocess.run(
                command,
                check=False,
                capture_output=True,
                text=True,
                timeout=max(120.0, erase_count * args.erase_timeout_ms / 1000.0 + 60.0),
            )
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            if result.returncode != 0:
                raise RuntimeError(f"OpenOCD D7 session failed with exit code {result.returncode}")
            snapshots = parse_mailbox_snapshots(result.stdout + result.stderr)
            expected_commands = (
                args.stop_after_commands
                if args.stop_after_commands is not None
                else operation_count + 1
            )
            if len(snapshots) < expected_commands:
                raise RuntimeError(
                    f"D7 mailbox evidence is incomplete: expected at least "
                    f"{expected_commands} snapshots, got {len(snapshots)}"
                )
            for index, snapshot in enumerate(snapshots[-expected_commands:]):
                if (
                    snapshot[0] != MAILBOX_MAGIC
                    or snapshot[1] != MAILBOX_VERSION
                    or snapshot[2] != 0
                    or snapshot[3] != STATUS_DONE
                    or snapshot[10] != 0
                ):
                    raise RuntimeError(
                        f"D7 loader command {index} did not complete: "
                        f"status={snapshot[3]} error={snapshot[10]} "
                        f"mailbox={snapshot}"
                    )
            print(f"RA4_SWD_D7_MAILBOX_COMMANDS_PASS={expected_commands}")
            if args.stop_after_commands is not None:
                print(
                    "RA4_SWD_D7_CONTROLLED_STOP_PASS="
                    f"{args.stop_after_commands}"
                )
                return 0
            if not dump_path.is_file() or dump_path.stat().st_size != len(image):
                raise RuntimeError("D7 readback dump has an unexpected size")
            readback = dump_path.read_bytes()
            if readback != image:
                for offset, (expected, actual) in enumerate(zip(image, readback)):
                    if expected != actual:
                        raise RuntimeError(
                            f"D7 readback mismatch at "
                            f"0x{APPLICATION_FLASH_START + offset:08X}: "
                            f"expected 0x{expected:02X}, got 0x{actual:02X}"
                        )
                raise RuntimeError("D7 readback length mismatch")
            print(f"RA4_SWD_D7_READBACK_SHA256={hashlib.sha256(readback).hexdigest()}")
            print("BL616CL_RA4_SWD_D7_FLASH_VERIFY_PASS")
            print(capture_scratch(bridge_port), end="")
            print("BL616CL_RA4_SWD_D7_RESET_RUN_PASS")
        finally:
            if not args.skip_restore_scan:
                restore = [
                    sys.executable,
                    str(DEFAULT_RESTORE),
                    "--bridge-port",
                    bridge_port,
                    "--capture-timeout-ms",
                    "15000",
                ]
                print("RESTORE_COMMAND=" + " ".join(restore))
                restore_result = subprocess.run(restore, check=False)
                if restore_result.returncode != 0:
                    raise RuntimeError("standard scan Sketch restoration failed")
    print("BL616CL_RA4_SWD_D7_FLASH_PROGRAM_SESSION_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(run())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
