#!/usr/bin/env python3
"""Capture a redacted ESP32-S3 STA reconnect cycle after a controlled AP outage."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

import serial

import run_esp32_sta_baseline_gate as baseline
from sta_reconnect_gate_support import (
    observe_statuses,
    require_hook,
    require_outage_statuses,
    run_hook,
    status_sequence_text,
)


def write_reconnect(
    link: serial.Serial,
    enabled: bool,
    response_timeout_seconds: float,
    label: str,
) -> None:
    request = b"AT+RECONNECT=1\r\n" if enabled else b"AT+RECONNECT=0\r\n"
    baseline.require_exact(
        baseline.transact(link, request, response_timeout_seconds),
        baseline.RECONNECT_WRITE_RESPONSE,
        label,
    )


def best_effort_cleanup(
    link: serial.Serial,
    response_timeout_seconds: float,
    original_reconnect_enabled: bool | None,
    ap_recovery_hook: Path | None,
    ap_was_taken_offline: bool,
    ap_hook_timeout_seconds: float,
    ap_control_socket: str | None,
) -> None:
    if ap_was_taken_offline and ap_recovery_hook is not None:
        try:
            run_hook(
                ap_recovery_hook,
                "recovery",
                ap_hook_timeout_seconds,
                ap_control_socket,
            )
            print("ESP32_STA_RECONNECT_CLEANUP_AP_RECOVERY_HOOK_PASS")
        except RuntimeError:
            print("ESP32_STA_RECONNECT_CLEANUP_AP_RECOVERY_HOOK_FAILED", file=sys.stderr)

    try:
        write_reconnect(link, False, response_timeout_seconds, "cleanup RECONNECT disable")
        print("ESP32_STA_RECONNECT_CLEANUP_RECONNECT_DISABLED")
    except (OSError, RuntimeError, serial.SerialException):
        print("ESP32_STA_RECONNECT_CLEANUP_RECONNECT_DISABLE_FAILED", file=sys.stderr)

    try:
        baseline.require_exact(
            baseline.transact(link, baseline.DISCONNECT_REQUEST, response_timeout_seconds),
            baseline.DISCONNECT_RESPONSE,
            "cleanup DISCONNECT",
        )
        print("ESP32_STA_RECONNECT_CLEANUP_DISCONNECT_SENT")
    except (OSError, RuntimeError, serial.SerialException):
        print("ESP32_STA_RECONNECT_CLEANUP_DISCONNECT_FAILED", file=sys.stderr)

    if original_reconnect_enabled is not None:
        try:
            write_reconnect(
                link,
                original_reconnect_enabled,
                response_timeout_seconds,
                "cleanup RECONNECT restore",
            )
            print("ESP32_STA_RECONNECT_CLEANUP_RECONNECT_RESTORED")
        except (OSError, RuntimeError, serial.SerialException):
            print("ESP32_STA_RECONNECT_CLEANUP_RECONNECT_RESTORE_FAILED", file=sys.stderr)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run a credential-safe ESP32-S3 STA reconnect baseline using controlled AP hooks."
    )
    parser.add_argument("--port", default=baseline.DEFAULT_PORT)
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--fqbn", default=baseline.DEFAULT_FQBN)
    parser.add_argument("--response-timeout-ms", type=int, default=2000)
    parser.add_argument("--connect-timeout-ms", type=int, default=30000)
    parser.add_argument("--outage-observe-ms", type=int, default=30000)
    parser.add_argument("--recovery-timeout-ms", type=int, default=30000)
    parser.add_argument("--poll-interval-ms", type=int, default=250)
    parser.add_argument("--scan-startup-timeout-ms", type=int, default=100)
    parser.add_argument("--scan-capture-timeout-ms", type=int, default=45000)
    parser.add_argument("--ap-hook-timeout-ms", type=int, default=15000)
    parser.add_argument(
        "--outage-hook",
        help="Executable local hook that makes the dedicated test AP unavailable.",
    )
    parser.add_argument(
        "--recovery-hook",
        help="Executable local hook that restores the dedicated test AP.",
    )
    parser.add_argument(
        "--ap-control-socket",
        help="Pass this private SoftAP controller socket to the outage/recovery hooks.",
    )
    parser.add_argument(
        "--skip-upload",
        action="store_true",
        help="Assume the temporary RA4M1 proxy is already running.",
    )
    parser.add_argument(
        "--keep-proxy",
        action="store_true",
        help="Do not restore ra4-wifis3-scan-smoke after the test.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    timings = (
        args.response_timeout_ms,
        args.connect_timeout_ms,
        args.outage_observe_ms,
        args.recovery_timeout_ms,
        args.poll_interval_ms,
        args.scan_capture_timeout_ms,
        args.ap_hook_timeout_ms,
    )
    if any(value <= 0 for value in timings) or args.scan_startup_timeout_ms < 0:
        raise RuntimeError("invalid timeout")

    outage_hook = require_hook(args.outage_hook, "outage")
    recovery_hook = require_hook(args.recovery_hook, "recovery")
    ssid = baseline.require_ascii_credential(
        "BL616CL_STA_SSID", os.environ.get("BL616CL_STA_SSID"), 32
    )
    password = baseline.require_ascii_credential(
        "BL616CL_STA_PASSWORD", os.environ.get("BL616CL_STA_PASSWORD"), 64
    )
    beginsta_request = baseline.build_beginsta_request(ssid, password)
    response_timeout_seconds = args.response_timeout_ms / 1000.0
    hook_timeout_seconds = args.ap_hook_timeout_ms / 1000.0

    baseline.require_port_identity(args.port)
    print(f"ESP32_BASELINE_PORT={args.port}")
    print("Target: original ESP32-S3 UNO R4 RA4M1 application Flash only.")
    print("Target: ESP32-S3 firmware Flash is untouched.")
    print("Privacy: credentials, AT payloads, and AP hook output stay out of logs.")

    link: serial.Serial | None = None
    original_reconnect_enabled: bool | None = None
    proxy_uploaded = False
    ap_was_taken_offline = False
    primary_error: Exception | None = None
    connected_ms: float | None = None
    reconnected_ms: float | None = None
    connect_statuses: list[int] = []
    outage_statuses: list[int] = []
    recovery_statuses: list[int] = []

    try:
        if args.skip_upload:
            print("ESP32_RA4_PROXY_UPLOAD_SKIPPED")
        else:
            baseline.compile_and_upload(
                args, baseline.PROXY_SKETCH, baseline.PROXY_BUILD_DIR, "ESP32_RA4_PROXY"
            )
            proxy_uploaded = True
            time.sleep(2.0)

        link = baseline.open_serial(args.port, 0.1)
        baseline.require_exact(
            baseline.transact(link, baseline.AT_REQUEST, response_timeout_seconds),
            baseline.AT_RESPONSE,
            "AT",
        )
        baseline.require_exact(
            baseline.transact(link, baseline.FIRMWARE_REQUEST, response_timeout_seconds),
            baseline.FIRMWARE_RESPONSE,
            "FWVERSION",
        )
        baseline.require_exact(
            baseline.transact(link, baseline.WIFI_MODE_REQUEST, response_timeout_seconds),
            baseline.WIFI_MODE_STA_RESPONSE,
            "WIFIMODE",
        )
        original_reconnect_enabled = baseline.parse_reconnect(
            baseline.transact(link, baseline.RECONNECT_READ_REQUEST, response_timeout_seconds)
        )
        write_reconnect(link, True, response_timeout_seconds, "RECONNECT enable")
        print("ESP32_STA_RECONNECT_ENABLED")

        baseline.validate_beginsta(
            baseline.transact(link, beginsta_request, response_timeout_seconds)
        )
        print("ESP32_STA_RECONNECT_ASSOCIATION_REQUEST_SENT")
        connected_ms, connect_statuses = baseline.poll_status(
            link,
            args.connect_timeout_ms / 1000.0,
            response_timeout_seconds,
            expected=3,
        )
        if connected_ms is None:
            raise RuntimeError("ESP32-S3 baseline association did not reach GETSTATUS=3")

        ap_was_taken_offline = True
        run_hook(
            outage_hook,
            "outage",
            hook_timeout_seconds,
            args.ap_control_socket,
        )
        print("ESP32_STA_RECONNECT_AP_OUTAGE_HOOK_PASS")
        outage_statuses = observe_statuses(
            lambda: baseline.parse_status(
                baseline.transact(link, baseline.STATUS_REQUEST, response_timeout_seconds)
            ),
            args.outage_observe_ms / 1000.0,
            args.poll_interval_ms / 1000.0,
            time.monotonic,
            time.sleep,
        )
        require_outage_statuses(outage_statuses)

        run_hook(
            recovery_hook,
            "recovery",
            hook_timeout_seconds,
            args.ap_control_socket,
        )
        ap_was_taken_offline = False
        print("ESP32_STA_RECONNECT_AP_RECOVERY_HOOK_PASS")
        reconnected_ms, recovery_statuses = baseline.poll_status(
            link,
            args.recovery_timeout_ms / 1000.0,
            response_timeout_seconds,
            expected=3,
        )
        if reconnected_ms is None:
            raise RuntimeError("ESP32-S3 baseline reconnect did not return to GETSTATUS=3")
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        primary_error = error
    finally:
        if link is not None and link.is_open:
            best_effort_cleanup(
                link,
                response_timeout_seconds,
                original_reconnect_enabled,
                recovery_hook,
                ap_was_taken_offline,
                hook_timeout_seconds,
                args.ap_control_socket,
            )
            link.close()

        if proxy_uploaded and not args.keep_proxy:
            try:
                baseline.restore_scan_smoke(args)
            except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
                if primary_error is None:
                    primary_error = error
                else:
                    print("ESP32_RA4_SCAN_RESTORE_FAILED", file=sys.stderr)

    if primary_error is not None:
        raise primary_error
    assert connected_ms is not None
    assert reconnected_ms is not None
    print(f"ESP32_STA_RECONNECT_CONNECT_STATUSES={status_sequence_text(connect_statuses)}")
    print(f"ESP32_STA_RECONNECT_CONNECTED_MS={connected_ms:.3f}")
    print(f"ESP32_STA_RECONNECT_OUTAGE_STATUSES={status_sequence_text(outage_statuses)}")
    print(f"ESP32_STA_RECONNECT_RECOVERY_STATUSES={status_sequence_text(recovery_statuses)}")
    print(f"ESP32_STA_RECONNECT_RECOVERED_MS={reconnected_ms:.3f}")
    print("ESP32_STA_RECONNECT_BASELINE_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
