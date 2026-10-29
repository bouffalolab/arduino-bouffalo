#!/usr/bin/env python3
"""Start and stop an ephemeral ESP32-S3 SoftAP without exposing its credentials."""

from __future__ import annotations

import argparse
import secrets
import subprocess
import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_esp32_sta_baseline_gate as baseline


SOFTAP_START_RESPONSE_PREFIX = b"+BEGINSOFTAP: "
SOFTAP_START_RESPONSE_SUFFIX = b"\r\nOK\r\n"
SOFTAP_STOP_REQUEST = b"AT+DISCONNECTSOFTAP=1\r\n"
TERMINAL = b"OK\r\n"


def quote_at_argument(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def build_softap_start_request(ssid: str, password: str, channel: int) -> bytes:
    if not ssid or len(ssid.encode("ascii", "strict")) > 32:
        raise RuntimeError("SoftAP SSID must contain 1 to 32 ASCII bytes")
    if not password or not 8 <= len(password.encode("ascii", "strict")) <= 63:
        raise RuntimeError("SoftAP password must contain 8 to 63 ASCII bytes")
    if not 1 <= channel <= 13:
        raise RuntimeError("SoftAP channel must be in the range 1 to 13")
    return (
        "AT+BEGINSOFTAP="
        f"{quote_at_argument(ssid)},{quote_at_argument(password)},{channel},0,1\r\n"
    ).encode("ascii")


def require_softap_start_response(response: bytes) -> None:
    if (
        not response.startswith(SOFTAP_START_RESPONSE_PREFIX)
        or not response.endswith(SOFTAP_START_RESPONSE_SUFFIX)
    ):
        raise RuntimeError("BEGINSOFTAP did not match the expected ESP32-S3 framing")
    payload = response[
        len(SOFTAP_START_RESPONSE_PREFIX) : -len(SOFTAP_START_RESPONSE_SUFFIX)
    ]
    if payload != b"1":
        raise RuntimeError("BEGINSOFTAP did not report a successful immediate result")


def poll_for_status(
    link: serial.Serial,
    expected_status: int,
    timeout_seconds: float,
    response_timeout_seconds: float,
) -> tuple[float | None, list[int]]:
    return baseline.poll_status(
        link,
        timeout_seconds,
        response_timeout_seconds,
        expected=expected_status,
    )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run an ephemeral, credential-redacted ESP32-S3 SoftAP start/stop smoke."
    )
    parser.add_argument("--port", default=baseline.DEFAULT_PORT)
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--fqbn", default=baseline.DEFAULT_FQBN)
    parser.add_argument("--response-timeout-ms", type=int, default=2000)
    parser.add_argument("--status-timeout-ms", type=int, default=10000)
    parser.add_argument("--channel", type=int, default=1)
    parser.add_argument("--scan-startup-timeout-ms", type=int, default=100)
    parser.add_argument("--scan-capture-timeout-ms", type=int, default=45000)
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if (
        args.response_timeout_ms <= 0
        or args.status_timeout_ms <= 0
        or args.scan_startup_timeout_ms < 0
        or args.scan_capture_timeout_ms <= 0
    ):
        raise RuntimeError("invalid timeout")

    request = build_softap_start_request(
        "uno-r4-" + secrets.token_hex(4),
        secrets.token_urlsafe(18),
        args.channel,
    )
    response_timeout_seconds = args.response_timeout_ms / 1000.0
    status_timeout_seconds = args.status_timeout_ms / 1000.0

    baseline.require_port_identity(args.port)
    print(f"ESP32_BASELINE_PORT={args.port}")
    print("Target: original ESP32-S3 UNO R4 RA4M1 application Flash only.")
    print("Target: ESP32-S3 firmware Flash is untouched.")
    print("Privacy: generated SoftAP credentials and raw AT responses stay in memory.")

    link: serial.Serial | None = None
    proxy_uploaded = False
    softap_started = False
    primary_error: Exception | None = None
    started_ms: float | None = None
    stopped_ms: float | None = None
    started_statuses: list[int] = []
    stopped_statuses: list[int] = []

    try:
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
        baseline.parse_firmware(
            baseline.transact(link, baseline.FIRMWARE_REQUEST, response_timeout_seconds)
        )
        require_softap_start_response(
            baseline.transact(link, request, response_timeout_seconds)
        )
        softap_started = True
        started_ms, started_statuses = poll_for_status(
            link, 7, status_timeout_seconds, response_timeout_seconds
        )
        if started_ms is None:
            raise RuntimeError("ESP32-S3 SoftAP did not reach GETSTATUS=7")

        baseline.require_exact(
            baseline.transact(link, SOFTAP_STOP_REQUEST, response_timeout_seconds),
            TERMINAL,
            "DISCONNECTSOFTAP",
        )
        softap_started = False
        stopped_ms, stopped_statuses = poll_for_status(
            link, 0, status_timeout_seconds, response_timeout_seconds
        )
        if stopped_ms is None:
            raise RuntimeError("ESP32-S3 SoftAP did not return to GETSTATUS=0")
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        primary_error = error
    finally:
        if link is not None and link.is_open:
            if softap_started:
                try:
                    baseline.require_exact(
                        baseline.transact(link, SOFTAP_STOP_REQUEST, response_timeout_seconds),
                        TERMINAL,
                        "cleanup DISCONNECTSOFTAP",
                    )
                    print("ESP32_SOFTAP_CLEANUP_STOP_SENT")
                except (OSError, RuntimeError, serial.SerialException):
                    print("ESP32_SOFTAP_CLEANUP_STOP_FAILED", file=sys.stderr)
            link.close()

        if proxy_uploaded:
            try:
                baseline.restore_scan_smoke(args)
            except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
                if primary_error is None:
                    primary_error = error
                else:
                    print("ESP32_RA4_SCAN_RESTORE_FAILED", file=sys.stderr)

    if primary_error is not None:
        raise primary_error
    assert started_ms is not None
    assert stopped_ms is not None
    print(f"ESP32_SOFTAP_START_STATUSES={','.join(map(str, started_statuses))}")
    print(f"ESP32_SOFTAP_STARTED_MS={started_ms:.3f}")
    print(f"ESP32_SOFTAP_STOP_STATUSES={','.join(map(str, stopped_statuses))}")
    print(f"ESP32_SOFTAP_STOPPED_MS={stopped_ms:.3f}")
    print("ESP32_SOFTAP_SMOKE_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
