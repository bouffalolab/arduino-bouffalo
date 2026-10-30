#!/usr/bin/env python3
"""Validate a read-only OpenOCD CMSIS-DAP session on the paired BL616CL board."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from uno_r4_usb_identity import resolve_paired_bl616cl_port, tty_properties


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_ARDUINO15 = Path.home() / ".arduino15" / "packages" / "arduino"
CPUID = 0x410FC241


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--openocd", type=Path)
    parser.add_argument("--target-config", type=Path)
    parser.add_argument("--scripts-dir", type=Path)
    parser.add_argument("--adapter-khz", type=int, default=100)
    parser.add_argument("--skip-scan-recovery", action="store_true")
    parser.add_argument("--capture-timeout-ms", type=int, default=45000)
    return parser.parse_args()


def require_final_carrier(port: str) -> None:
    properties = tty_properties(port)
    if (
        properties.get("ID_VENDOR_ID", "").lower() != "2341"
        or properties.get("ID_MODEL_ID", "").lower() != "1002"
        or properties.get("ID_USB_INTERFACE_NUM", "").lower() != "00"
        or properties.get("ID_SERIAL_SHORT") != "BL616CL-PROVISIONAL"
    ):
        raise RuntimeError(f"{port} is not the final-carrier BL616CL CDC interface")


def find_single(description: str, candidates: list[Path]) -> Path:
    existing = [candidate for candidate in candidates if candidate.is_file()]
    if len(existing) != 1:
        found = ", ".join(str(candidate) for candidate in existing) or "none"
        raise RuntimeError(f"expected exactly one {description}, found: {found}")
    return existing[0]


def resolve_openocd_paths(args: argparse.Namespace) -> tuple[Path, Path, Path]:
    package_roots = sorted(DEFAULT_ARDUINO15.glob("tools/openocd/*"))
    tool_roots = [root for root in package_roots if (root / "bin" / "openocd").is_file()]
    if args.openocd is not None:
        openocd = args.openocd.expanduser().resolve()
    else:
        openocd = find_single(
            "Arduino OpenOCD executable",
            [root / "bin" / "openocd" for root in tool_roots],
        )
    if not openocd.is_file():
        raise RuntimeError(f"OpenOCD executable does not exist: {openocd}")

    if args.scripts_dir is not None:
        scripts_dir = args.scripts_dir.expanduser().resolve()
    else:
        scripts_dir = openocd.parents[1] / "share" / "openocd" / "scripts"
    if not scripts_dir.is_dir():
        raise RuntimeError(f"OpenOCD scripts directory does not exist: {scripts_dir}")

    if args.target_config is not None:
        target_config = args.target_config.expanduser().resolve()
    else:
        target_config = find_single(
            "Arduino Renesas RA4M1 target config",
            sorted(
                DEFAULT_ARDUINO15.glob(
                    "hardware/renesas_uno/*/debugger/R7FA4M1AB.cfg"
                )
            ),
        )
    if not target_config.is_file():
        raise RuntimeError(f"OpenOCD RA4M1 target config does not exist: {target_config}")
    return openocd, scripts_dir, target_config


def require_openocd_result(output: str) -> None:
    expected = (
        "CMSIS-DAP: Serial# = BL616CL-PROVISIONAL",
        "SWD DPIDR 0x5ba02477",
        "target halted due to debug-request",
        f"0xe000ed00: {CPUID:08x}",
        "0x00000000:",
        "0x20000000:",
        "shutdown command invoked",
    )
    missing = [marker for marker in expected if marker.lower() not in output.lower()]
    if missing:
        raise RuntimeError(
            "OpenOCD D5 output did not contain required markers: " + ", ".join(missing)
        )

    cpuid_matches = re.findall(r"0xe000ed00:\s+([0-9a-fA-F]{8})", output)
    if [int(value, 16) for value in cpuid_matches] != [CPUID]:
        raise RuntimeError(f"unexpected OpenOCD CPUID output: {cpuid_matches!r}")


def main() -> int:
    args = parse_arguments()
    if args.adapter_khz <= 0 or args.capture_timeout_ms <= 0:
        raise RuntimeError("adapter speed and capture timeout must be positive")

    bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier(bridge_port)
    openocd, scripts_dir, target_config = resolve_openocd_paths(args)

    command = [
        str(openocd),
        "-s",
        str(scripts_dir),
        "-f",
        "interface/cmsis-dap.cfg",
        "-c",
        "cmsis_dap_serial BL616CL-PROVISIONAL",
        "-f",
        str(target_config),
        "-c",
        f"adapter speed {args.adapter_khz}",
        "-c",
        "init",
        "-c",
        "halt",
        "-c",
        "mdw 0xE000ED00 1",
        "-c",
        "mdw 0x00000000 4",
        "-c",
        "mdw 0x20000000 4",
        "-c",
        "resume",
        "-c",
        "reset run",
        "-c",
        "shutdown",
    ]
    print(f"BL616CL_FINAL_CARRIER_PORT={bridge_port}")
    print("Target: RA4M1 SWD halt/read/resume/reset-run only; no Flash is written.")
    print(f"OPENOCD_EXECUTABLE={openocd}")
    print(f"OPENOCD_TARGET_CONFIG={target_config}")
    print("COMMAND=" + " ".join(command))
    result = subprocess.run(command, check=False, capture_output=True, text=True)
    output = result.stdout + result.stderr
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    if result.returncode != 0:
        raise RuntimeError(f"OpenOCD D5 session failed with exit code {result.returncode}")
    require_openocd_result(output)
    print("BL616CL_RA4_SWD_D5_OPENOCD_SESSION_PASS")

    if not args.skip_scan_recovery:
        recovery = [
            sys.executable,
            str(ROOT_DIR / "tools" / "run_bl616cl_swd_d4_scan_recovery.py"),
            "--bridge-port",
            bridge_port,
            "--capture-timeout-ms",
            str(args.capture_timeout_ms),
        ]
        print("COMMAND=" + " ".join(recovery))
        if subprocess.run(recovery, check=False).returncode != 0:
            raise RuntimeError("D4 recovery/scan validation after OpenOCD failed")
        require_final_carrier(bridge_port)
        print("BL616CL_RA4_SWD_D5_OPENOCD_SCAN_RECOVERY_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
