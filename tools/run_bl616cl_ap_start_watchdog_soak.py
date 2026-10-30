#!/usr/bin/env python3
"""Repeat the BL616CL SoftAP watchdog recovery diagnostic without reflashing.

The paired BL616CL must already run an image built with
BL616CL_AP_START_DIAGNOSTIC_WATCHDOG=1. The first cycle installs temporary
RA4M1 AT proxy Sketches. Later cycles reuse them so repeated results measure
the watchdog recovery path rather than repeated SAM-BA uploads.

The tool deliberately leaves the proxy Sketches in place when a cycle fails.
That preserves the failure state for HID/SWD investigation. Run the normal
scan-smoke restore after collecting the desired evidence.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import inspect_bl616cl_gpio35 as telemetry
from uno_r4_usb_identity import (
    resolve_baseline_esp32_port,
    resolve_paired_bl616cl_port,
    resolve_relay_port,
)


ROOT_DIR = Path(__file__).resolve().parents[1]
SOFTAP_GATE = ROOT_DIR / "tools" / "run_bl616cl_softap_gate.py"


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cycles", type=int, default=3)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--relay-port", default="auto")
    parser.add_argument("--esp32-port", default="auto")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--response-timeout-ms", type=int, default=2000)
    parser.add_argument("--status-timeout-ms", type=int, default=20000)
    parser.add_argument("--runtime-settle-ms", type=int, default=300)
    parser.add_argument("--watchdog-recovery-timeout-ms", type=int, default=20000)
    parser.add_argument(
        "--prepare-hidraw-via-wsl-root",
        action="store_true",
        help="Grant temporary read/write access to the current WSL hidraw node.",
    )
    parser.add_argument("--wsl-distribution", default="Ubuntu-22.04")
    return parser.parse_args()


def read_bootstrap_telemetry() -> None:
    """Best-effort failure snapshot; do not hide the primary cycle result."""
    result = subprocess.run(
        [sys.executable, str(ROOT_DIR / "tools" / "inspect_bl616cl_gpio35.py"),
         "--ra4-bootstrap"],
        check=False,
        text=True,
    )
    if result.returncode != 0:
        print("AP_START_WATCHDOG_SOAK_BOOTSTRAP_TELEMETRY_FAILED", file=sys.stderr)


def main() -> int:
    args = parse_arguments()
    if args.cycles <= 0:
        raise RuntimeError("--cycles must be positive")
    if (
        args.response_timeout_ms <= 0
        or args.status_timeout_ms <= 0
        or args.runtime_settle_ms < 0
        or args.watchdog_recovery_timeout_ms <= 0
    ):
        raise RuntimeError("invalid timeout")

    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    args.relay_port = resolve_relay_port(args.relay_port)
    args.esp32_port = resolve_baseline_esp32_port(args.esp32_port)
    if not SOFTAP_GATE.is_file():
        raise RuntimeError(f"SoftAP gate does not exist: {SOFTAP_GATE}")

    print(
        "AP_START_WATCHDOG_SOAK="
        f"cycles={args.cycles} bridge={args.bridge_port} relay={args.relay_port} "
        f"esp32={args.esp32_port}"
    )
    print("Privacy: generated SoftAP credentials remain inside each child process.")

    completed = 0
    for cycle in range(1, args.cycles + 1):
        command = [
            sys.executable,
            str(SOFTAP_GATE),
            "--bridge-port",
            args.bridge_port,
            "--relay-port",
            args.relay_port,
            "--esp32-port",
            args.esp32_port,
            "--arduino-cli",
            args.arduino_cli,
            "--response-timeout-ms",
            str(args.response_timeout_ms),
            "--status-timeout-ms",
            str(args.status_timeout_ms),
            "--runtime-settle-ms",
            str(args.runtime_settle_ms),
            "--watchdog-recovery-timeout-ms",
            str(args.watchdog_recovery_timeout_ms),
            "--wsl-distribution",
            args.wsl_distribution,
            "--expect-ap-start-watchdog-reset",
            "--keep-proxy",
        ]
        if args.prepare_hidraw_via_wsl_root:
            command.append("--prepare-hidraw-via-wsl-root")
        if cycle > 1:
            command.append("--skip-proxy-upload")

        print(f"AP_START_WATCHDOG_SOAK_CYCLE_BEGIN={cycle}")
        started = time.monotonic()
        result = subprocess.run(command, check=False)
        elapsed_ms = int((time.monotonic() - started) * 1000)
        if result.returncode != 0:
            print(
                "AP_START_WATCHDOG_SOAK_CYCLE_FAIL="
                f"index={cycle} exit={result.returncode} elapsed_ms={elapsed_ms}",
                file=sys.stderr,
            )
            read_bootstrap_telemetry()
            print(
                "AP_START_WATCHDOG_SOAK_SUMMARY="
                f"passed={completed} failed_index={cycle} requested={args.cycles}",
                file=sys.stderr,
            )
            raise RuntimeError("watchdog recovery soak stopped at the first failed cycle")

        completed += 1
        print(
            "AP_START_WATCHDOG_SOAK_CYCLE_PASS="
            f"index={cycle} elapsed_ms={elapsed_ms}"
        )

    print(
        "AP_START_WATCHDOG_SOAK_PASS="
        f"completed={completed} requested={args.cycles}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
