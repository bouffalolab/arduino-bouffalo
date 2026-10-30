#!/usr/bin/env python3
"""Run bounded repeated D7 RA4M1 SWD Flash program/verify/reset-run sessions."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
D7_RUNNER = ROOT / "tools/run_bl616cl_swd_d7_flash_program.py"


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--cycles", type=int, default=2)
    parser.add_argument("--adapter-khz", type=int, default=100)
    parser.add_argument("--interrupt-after-commands", type=int, default=5)
    parser.add_argument(
        "--long-session",
        action="store_true",
        help="require at least five complete SWD program/verify cycles",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if args.cycles < 2:
        raise RuntimeError("D8 repeat gate requires at least two cycles")
    if args.adapter_khz <= 0:
        raise RuntimeError("adapter speed must be positive")
    if args.interrupt_after_commands <= 0:
        raise RuntimeError("interrupt command count must be positive")
    if args.long_session and args.cycles < 5:
        raise RuntimeError("--long-session requires --cycles >= 5")

    def run_d7(*extra: str) -> None:
        command = [
            sys.executable,
            str(D7_RUNNER),
            "--bridge-port",
            args.bridge_port,
            "--adapter-khz",
            str(args.adapter_khz),
            "--skip-restore-scan",
            *extra,
        ]
        print("COMMAND=" + " ".join(command))
        result = subprocess.run(command, check=False)
        if result.returncode != 0:
            raise RuntimeError("D8 D7 sub-session failed")

    try:
        for cycle in range(1, args.cycles + 1):
            print(f"RA4_SWD_D8_REPEAT_CYCLE_BEGIN={cycle}")
            run_d7()
            print(f"RA4_SWD_D8_REPEAT_CYCLE_PASS={cycle}")

        print(f"RA4_SWD_D8_REPEAT_PROGRAM_PASS={args.cycles}")
        print(f"RA4_SWD_D8_CONTROLLED_INTERRUPT_BEGIN={args.interrupt_after_commands}")
        run_d7("--stop-after-commands", str(args.interrupt_after_commands))
        print(f"RA4_SWD_D8_CONTROLLED_INTERRUPT_PASS={args.interrupt_after_commands}")
        print("RA4_SWD_D8_RECOVERY_PROGRAM_BEGIN")
        run_d7()
    finally:
        # Host BOSSA must not repair the interrupted image before SWD recovery.
        command = [sys.executable, str(ROOT / "tools/run_ra4_wifis3_scan_final_carrier.py"),
                   "--bridge-port", args.bridge_port]
        if subprocess.run(command, check=False).returncode != 0:
            raise RuntimeError("D8 final scan restoration failed")
    print("RA4_SWD_D8_RECOVERY_PROGRAM_PASS")
    print("RA4_SWD_D8_CONTROLLED_RECOVERY_PASS")
    if args.long_session:
        print(f"RA4_SWD_D8_LONG_SESSION_PASS={args.cycles}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
