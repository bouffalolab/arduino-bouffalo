#!/usr/bin/env python3
"""Capture a redacted BL616CL STA reconnect cycle after a controlled AP outage.

The caller provides two executable local hooks. The outage hook makes only the
dedicated test AP unavailable; the recovery hook makes that same AP available
again. Hook output is intentionally discarded and STA credentials are removed
from the hook environment.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

import serial

import run_ra4_sta_association_gate as association
from sta_reconnect_gate_support import (
    observe_statuses,
    require_hook,
    require_outage_statuses,
    run_hook,
    status_sequence_text,
)
from uno_r4_usb_identity import resolve_paired_bl616cl_port, resolve_relay_port

def read_status(
    bridge: serial.Serial, reference: serial.Serial, response_timeout_seconds: float
) -> int:
    response = association.transact(
        bridge,
        reference,
        association.STATUS_REQUEST,
        response_timeout_seconds,
    )
    return association.parse_status_response(response)


def write_reconnect(
    bridge: serial.Serial,
    reference: serial.Serial,
    enabled: bool,
    response_timeout_seconds: float,
    label: str,
) -> None:
    request = b"AT+RECONNECT=1\r\n" if enabled else b"AT+RECONNECT=0\r\n"
    response = association.transact(bridge, reference, request, response_timeout_seconds)
    association.require_exact_response(response, association.RECONNECT_WRITE_RESPONSE, label)


def best_effort_cleanup(
    bridge: serial.Serial,
    reference: serial.Serial,
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
            print("STA_RECONNECT_CLEANUP_AP_RECOVERY_HOOK_PASS")
        except RuntimeError:
            print("STA_RECONNECT_CLEANUP_AP_RECOVERY_HOOK_FAILED", file=sys.stderr)

    try:
        write_reconnect(
            bridge,
            reference,
            False,
            response_timeout_seconds,
            "cleanup RECONNECT disable",
        )
        print("STA_RECONNECT_CLEANUP_RECONNECT_DISABLED")
    except (OSError, RuntimeError, serial.SerialException):
        print("STA_RECONNECT_CLEANUP_RECONNECT_DISABLE_FAILED", file=sys.stderr)

    try:
        response = association.transact(
            bridge,
            reference,
            association.DISCONNECT_REQUEST,
            response_timeout_seconds,
        )
        association.require_exact_response(response, association.DISCONNECT_RESPONSE, "cleanup DISCONNECT")
        print("STA_RECONNECT_CLEANUP_DISCONNECT_SENT")
    except (OSError, RuntimeError, serial.SerialException):
        print("STA_RECONNECT_CLEANUP_DISCONNECT_FAILED", file=sys.stderr)

    if original_reconnect_enabled is not None:
        try:
            write_reconnect(
                bridge,
                reference,
                original_reconnect_enabled,
                response_timeout_seconds,
                "cleanup RECONNECT restore",
            )
            print("STA_RECONNECT_CLEANUP_RECONNECT_RESTORED")
        except (OSError, RuntimeError, serial.SerialException):
            print("STA_RECONNECT_CLEANUP_RECONNECT_RESTORE_FAILED", file=sys.stderr)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run a credential-safe BL616CL STA reconnect gate using controlled AP hooks."
    )
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--relay-port", default="auto")
    parser.add_argument(
        "--reference-port",
        dest="relay_port",
        default=argparse.SUPPRESS,
        help=argparse.SUPPRESS,
    )
    parser.add_argument("--legacy-reference-port")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--response-timeout-ms", type=int, default=2000)
    parser.add_argument("--connect-timeout-ms", type=int, default=30000)
    parser.add_argument("--outage-observe-ms", type=int, default=30000)
    parser.add_argument("--recovery-timeout-ms", type=int, default=30000)
    parser.add_argument("--poll-interval-ms", type=int, default=250)
    parser.add_argument("--runtime-settle-ms", type=int, default=2000)
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
        "--upload-proxy",
        action="store_true",
        help="Build and upload the temporary RA4M1 Serial <-> Serial2 proxy first.",
    )
    parser.add_argument(
        "--keep-proxy",
        action="store_true",
        help="Do not restore ra4-wifis3-scan-smoke after the test.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    args.relay_port = resolve_relay_port(args.relay_port)
    timings = (
        args.response_timeout_ms,
        args.connect_timeout_ms,
        args.outage_observe_ms,
        args.recovery_timeout_ms,
        args.poll_interval_ms,
        args.ap_hook_timeout_ms,
    )
    if any(value <= 0 for value in timings) or args.runtime_settle_ms < 0:
        raise RuntimeError("timeouts must be positive")

    outage_hook = require_hook(args.outage_hook, "outage")
    recovery_hook = require_hook(args.recovery_hook, "recovery")
    ssid = association.require_ascii_credential(
        "BL616CL_STA_SSID",
        os.environ.get("BL616CL_STA_SSID"),
        32,
        required=True,
    )
    password = association.require_ascii_credential(
        "BL616CL_STA_PASSWORD",
        os.environ.get("BL616CL_STA_PASSWORD"),
        64,
        required=False,
    )
    assert ssid is not None
    beginsta_request = association.build_beginsta_request(ssid, password)
    response_timeout_seconds = args.response_timeout_ms / 1000.0
    hook_timeout_seconds = args.ap_hook_timeout_ms / 1000.0

    association.require_port_identity(args.bridge_port, "2341", "1002", "00")
    association.require_port_identity(args.relay_port, "0403", "6010", "00")
    if args.legacy_reference_port is not None:
        association.require_port_identity(
            args.legacy_reference_port, "0403", "6010", "00"
        )
    print(f"Bridge:    {args.bridge_port} (2341:1002 interface 00)")
    print(f"Relay:     {args.relay_port} (0403:6010 interface 00)")
    print("Target: RA4M1 application Flash only when --upload-proxy or scan restoration runs.")
    print("Target: BL616CL Flash is untouched.")
    print("Relay route: RA4M1 UART (RTS=False, physical RTS# HIGH, relay released).")
    print("Privacy: credentials, AT payloads, and AP hook output stay out of logs.")

    bridge: serial.Serial | None = None
    reference: serial.Serial | None = None
    route = None
    original_reconnect_enabled: bool | None = None
    proxy_active = False
    ap_was_taken_offline = False
    primary_error: Exception | None = None
    connected_ms: float | None = None
    reconnected_ms: float | None = None
    connect_statuses: list[int] = []
    outage_statuses: list[int] = []
    recovery_statuses: list[int] = []

    try:
        if args.upload_proxy and args.skip_upload:
            raise RuntimeError("--upload-proxy and --skip-upload are mutually exclusive")
        if args.upload_proxy:
            association.run_proxy_upload(args)
            proxy_active = True
        elif args.skip_upload:
            print("RA4_PROXY_UPLOAD_SKIPPED")

        route = association.open_runtime_route(
            args.bridge_port,
            args.relay_port,
            args.legacy_reference_port,
            bridge_timeout=1.0,
            receiver_timeout=0.05,
        )
        bridge = route.bridge
        reference = route.receiver
        bridge.reset_input_buffer()
        reference.reset_input_buffer()

        response = association.transact_idempotent(
            bridge, reference, association.WIFI_MODE_REQUEST, response_timeout_seconds
        )
        association.require_exact_response(
            response, association.WIFI_MODE_STA_RESPONSE, "WIFIMODE preflight"
        )
        proxy_active = True
        original_reconnect_enabled = association.parse_reconnect_response(
            association.transact_idempotent(
                bridge, reference, association.RECONNECT_READ_REQUEST, response_timeout_seconds
            )
        )
        response = association.transact_idempotent(
            bridge,
            reference,
            b"AT+RECONNECT=1\r\n",
            response_timeout_seconds,
        )
        association.require_exact_response(
            response,
            association.RECONNECT_WRITE_RESPONSE,
            "RECONNECT enable",
        )
        print("STA_RECONNECT_ENABLED")

        response = association.transact(
            bridge, reference, beginsta_request, response_timeout_seconds
        )
        association.validate_beginsta_response(response)
        print("STA_RECONNECT_ASSOCIATION_REQUEST_SENT")
        connected_ms, connect_statuses = association.poll_for_status(
            bridge,
            reference,
            expected_status=3,
            timeout_seconds=args.connect_timeout_ms / 1000.0,
            response_timeout_seconds=response_timeout_seconds,
            poll_interval_seconds=args.poll_interval_ms / 1000.0,
        )
        if connected_ms is None:
            raise RuntimeError("STA association did not reach GETSTATUS=3")

        ap_was_taken_offline = True
        run_hook(
            outage_hook,
            "outage",
            hook_timeout_seconds,
            args.ap_control_socket,
        )
        print("STA_RECONNECT_AP_OUTAGE_HOOK_PASS")
        outage_statuses = observe_statuses(
            lambda: read_status(bridge, reference, response_timeout_seconds),
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
        print("STA_RECONNECT_AP_RECOVERY_HOOK_PASS")
        reconnected_ms, recovery_statuses = association.poll_for_status(
            bridge,
            reference,
            expected_status=3,
            timeout_seconds=args.recovery_timeout_ms / 1000.0,
            response_timeout_seconds=response_timeout_seconds,
            poll_interval_seconds=args.poll_interval_ms / 1000.0,
        )
        if reconnected_ms is None:
            raise RuntimeError("STA reconnect did not return to GETSTATUS=3")
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        primary_error = error
    finally:
        if bridge is not None and reference is not None:
            best_effort_cleanup(
                bridge,
                reference,
                response_timeout_seconds,
                original_reconnect_enabled,
                recovery_hook,
                ap_was_taken_offline,
                hook_timeout_seconds,
                args.ap_control_socket,
            )
        if route is not None:
            route.close()

        if proxy_active and not args.keep_proxy:
            try:
                association.restore_scan_smoke(args)
            except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
                if primary_error is None:
                    primary_error = error
                else:
                    print("RA4_SCAN_RESTORE_FAILED", file=sys.stderr)

    if primary_error is not None:
        raise primary_error
    assert connected_ms is not None
    assert reconnected_ms is not None
    print(f"STA_RECONNECT_CONNECT_STATUSES={status_sequence_text(connect_statuses)}")
    print(f"STA_RECONNECT_CONNECTED_MS={connected_ms:.3f}")
    print(f"STA_RECONNECT_OUTAGE_STATUSES={status_sequence_text(outage_statuses)}")
    print(f"STA_RECONNECT_RECOVERY_STATUSES={status_sequence_text(recovery_statuses)}")
    print(f"STA_RECONNECT_RECOVERED_MS={reconnected_ms:.3f}")
    print("RA4_AT_STA_RECONNECT_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
