#!/usr/bin/env python3
"""Require RA4M1 heartbeat restart across the BL616CL CMSIS-DAP D4 flow."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from run_bl616cl_raw_nreset_heartbeat import (
    capture_heartbeat,
    last_heartbeat_value,
    require_final_carrier,
)
from uno_r4_usb_identity import resolve_paired_bl616cl_port


ROOT_DIR = Path(__file__).resolve().parents[1]
TELEMETRY_RUNNER = ROOT_DIR / "tools" / "read_bl616cl_windows_telemetry.py"


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument(
        "--telemetry-page",
        choices=(
            "swd-halt-reset",
            "swd-d4-attach-readonly",
            "swd-d4-attach-readonly-reset",
            "swd-d4-release-power",
            "swd-d4-minimal-idcode-reset",
            "swd-d4-disconnect-delay",
            "swd-d4-halt-run-disconnect-delay",
            "swd-d4-no-reattach",
            "swd-d4-disconnect-reconnect",
        ),
        default="swd-halt-reset",
    )
    parser.add_argument("--capture-ms", type=int, default=8000)
    parser.add_argument("--maximum-capture-bytes", type=int, default=8192)
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if args.capture_ms <= 0 or args.maximum_capture_bytes <= 0:
        raise RuntimeError("capture limits must be positive")

    port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier(port)
    print(f"BL616CL_FINAL_CARRIER_PORT={port}")
    print("Target: RA4M1 D4 halt/run/raw nRESET and user UART only; no Flash is written.")
    before_runtime = capture_heartbeat(port, 3.0, args.maximum_capture_bytes)
    before_count = last_heartbeat_value(before_runtime)
    print(f"BL616CL_D4_HEARTBEAT_BEFORE={before_count}")

    command = [
        sys.executable,
        str(TELEMETRY_RUNNER),
        args.telemetry_page,
        "--bridge-port",
        port,
    ]
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("CMSIS-DAP D4 transport runner failed")
    require_final_carrier(port)

    runtime = capture_heartbeat(
        port,
        args.capture_ms / 1000.0,
        args.maximum_capture_bytes,
    )
    print("BL616CL_D4_HEARTBEAT_CAPTURE_BEGIN")
    print(runtime, end="" if runtime.endswith("\n") else "\n")
    print("BL616CL_D4_HEARTBEAT_CAPTURE_END")
    after_count = last_heartbeat_value(runtime)
    print(f"BL616CL_D4_HEARTBEAT_AFTER={after_count}")
    if after_count >= before_count:
        raise RuntimeError(
            "D4 did not restart the heartbeat counter: "
            f"before={before_count} after={after_count}"
        )
    print("BL616CL_RA4_SWD_D4_HEARTBEAT_RESTART_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
