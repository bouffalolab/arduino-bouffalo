#!/usr/bin/env python3
"""Keep a temporary ESP32-S3 SoftAP available for STA reconnect gates."""

from __future__ import annotations

import argparse
import os
import secrets
import signal
import socket
import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_esp32_softap_smoke as softap
import run_esp32_sta_baseline_gate as baseline


DEFAULT_CREDENTIAL_ENV = (
    Path.home() / ".config" / "uno-r4-wifi-bridge" / "esp32-softap-test.env"
)
DEFAULT_CONTROL_SOCKET = Path("/tmp/uno-r4-esp32-softap.sock")
TERMINAL = b"OK\r\n"
READY = b"OK\n"
ERROR = b"ERROR\n"


def generate_credentials() -> tuple[str, str]:
    return ("uno-r4-" + secrets.token_hex(4), secrets.token_urlsafe(18))


def write_private_credentials(path: Path, ssid: str, password: str) -> None:
    softap.build_softap_start_request(ssid, password, 1)
    if path.exists():
        raise RuntimeError(f"private credential file already exists: {path}")
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        os.fchmod(descriptor, 0o600)
        os.write(
            descriptor,
            f"BL616CL_STA_SSID={ssid}\nBL616CL_STA_PASSWORD={password}\n".encode(
                "ascii"
            ),
        )
    finally:
        os.close(descriptor)


def remove_private_file(path: Path) -> None:
    try:
        path.unlink()
    except FileNotFoundError:
        pass


def require_control_socket_path(path: Path) -> None:
    if path.parent != Path("/tmp"):
        raise RuntimeError("SoftAP control socket must be directly under /tmp")
    if path.exists():
        raise RuntimeError(f"SoftAP control socket already exists: {path}")


def create_control_socket(path: Path) -> socket.socket:
    require_control_socket_path(path)
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        listener.bind(str(path))
        os.chmod(path, 0o600)
        listener.listen(1)
        listener.settimeout(0.5)
        return listener
    except Exception:
        listener.close()
        remove_private_file(path)
        raise


def parse_control_request(payload: bytes) -> str:
    if payload == b"outage\n":
        return "outage"
    if payload == b"recovery\n":
        return "recovery"
    if payload == b"shutdown\n":
        return "shutdown"
    raise RuntimeError("unsupported SoftAP controller request")


def send_response(connection: socket.socket, payload: bytes) -> None:
    connection.sendall(payload)


def require_bridge_ready(link: serial.Serial, response_timeout_seconds: float) -> None:
    baseline.require_exact(
        baseline.transact(link, baseline.AT_REQUEST, response_timeout_seconds),
        baseline.AT_RESPONSE,
        "AT",
    )
    baseline.parse_firmware(
        baseline.transact(link, baseline.FIRMWARE_REQUEST, response_timeout_seconds)
    )


def set_softap_running(
    port: str,
    request: bytes,
    response_timeout_seconds: float,
    status_timeout_seconds: float,
) -> float:
    link = baseline.open_serial(port, 0.1)
    try:
        require_bridge_ready(link, response_timeout_seconds)
        softap.require_softap_start_response(
            baseline.transact(link, request, response_timeout_seconds)
        )
        elapsed_ms, _ = softap.poll_for_status(
            link, 7, status_timeout_seconds, response_timeout_seconds
        )
        if elapsed_ms is None:
            raise RuntimeError("ESP32-S3 SoftAP did not reach GETSTATUS=7")
        return elapsed_ms
    finally:
        link.close()


def set_softap_stopped(
    port: str,
    response_timeout_seconds: float,
    status_timeout_seconds: float,
) -> float:
    link = baseline.open_serial(port, 0.1)
    try:
        require_bridge_ready(link, response_timeout_seconds)
        baseline.require_exact(
            baseline.transact(
                link, softap.SOFTAP_STOP_REQUEST, response_timeout_seconds
            ),
            TERMINAL,
            "DISCONNECTSOFTAP",
        )
        elapsed_ms, _ = softap.poll_for_status(
            link, 0, status_timeout_seconds, response_timeout_seconds
        )
        if elapsed_ms is None:
            raise RuntimeError("ESP32-S3 SoftAP did not return to GETSTATUS=0")
        return elapsed_ms
    finally:
        link.close()


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run a private local ESP32-S3 SoftAP controller for reconnect gates."
    )
    parser.add_argument("--port", default=baseline.DEFAULT_PORT)
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--fqbn", default=baseline.DEFAULT_FQBN)
    parser.add_argument("--channel", type=int, default=1)
    parser.add_argument("--response-timeout-ms", type=int, default=2000)
    parser.add_argument("--status-timeout-ms", type=int, default=10000)
    parser.add_argument("--scan-startup-timeout-ms", type=int, default=100)
    parser.add_argument("--scan-capture-timeout-ms", type=int, default=45000)
    parser.add_argument("--credential-env", type=Path, default=DEFAULT_CREDENTIAL_ENV)
    parser.add_argument("--control-socket", type=Path, default=DEFAULT_CONTROL_SOCKET)
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if (
        args.channel < 1
        or args.channel > 13
        or args.response_timeout_ms <= 0
        or args.status_timeout_ms <= 0
        or args.scan_startup_timeout_ms < 0
        or args.scan_capture_timeout_ms <= 0
    ):
        raise RuntimeError("invalid controller option")

    baseline.require_port_identity(args.port)
    ssid, password = generate_credentials()
    request = softap.build_softap_start_request(ssid, password, args.channel)
    response_timeout_seconds = args.response_timeout_ms / 1000.0
    status_timeout_seconds = args.status_timeout_ms / 1000.0
    listener: socket.socket | None = None
    proxy_uploaded = False
    softap_running = False
    credential_written = False
    socket_created = False
    should_exit = False

    def stop_requested(_signal: int, _frame: object) -> None:
        nonlocal should_exit
        should_exit = True

    previous_sigint = signal.signal(signal.SIGINT, stop_requested)
    previous_sigterm = signal.signal(signal.SIGTERM, stop_requested)
    print(f"ESP32_SOFTAP_CONTROLLER_PORT={args.port}")
    print("Target: original ESP32-S3 UNO R4 RA4M1 application Flash only.")
    print("Target: ESP32-S3 firmware Flash is untouched.")
    print("Privacy: generated SoftAP credentials remain in a mode-600 local file only.")

    try:
        baseline.compile_and_upload(
            args, baseline.PROXY_SKETCH, baseline.PROXY_BUILD_DIR, "ESP32_RA4_PROXY"
        )
        proxy_uploaded = True
        time.sleep(2.0)

        set_softap_running(
            args.port, request, response_timeout_seconds, status_timeout_seconds
        )
        softap_running = True
        write_private_credentials(args.credential_env, ssid, password)
        credential_written = True
        listener = create_control_socket(args.control_socket)
        socket_created = True
        print(f"ESP32_SOFTAP_CONTROL_SOCKET={args.control_socket}")
        print(f"ESP32_SOFTAP_CREDENTIAL_ENV={args.credential_env}")
        print("ESP32_SOFTAP_CONTROLLER_READY")

        while not should_exit:
            try:
                connection, _ = listener.accept()
            except socket.timeout:
                continue
            with connection:
                try:
                    action = parse_control_request(connection.recv(64))
                    if action == "outage":
                        set_softap_stopped(
                            args.port,
                            response_timeout_seconds,
                            status_timeout_seconds,
                        )
                        softap_running = False
                    elif action == "recovery":
                        set_softap_running(
                            args.port,
                            request,
                            response_timeout_seconds,
                            status_timeout_seconds,
                        )
                        softap_running = True
                    else:
                        should_exit = True
                    send_response(connection, READY)
                except (OSError, RuntimeError, serial.SerialException):
                    send_response(connection, ERROR)
    finally:
        signal.signal(signal.SIGINT, previous_sigint)
        signal.signal(signal.SIGTERM, previous_sigterm)
        if listener is not None:
            listener.close()
        if socket_created:
            remove_private_file(args.control_socket)
        if softap_running:
            try:
                set_softap_stopped(
                    args.port, response_timeout_seconds, status_timeout_seconds
                )
                print("ESP32_SOFTAP_CONTROLLER_CLEANUP_STOP_PASS")
            except (OSError, RuntimeError, serial.SerialException):
                print("ESP32_SOFTAP_CONTROLLER_CLEANUP_STOP_FAILED", file=sys.stderr)
        if credential_written:
            remove_private_file(args.credential_env)
        if proxy_uploaded:
            baseline.restore_scan_smoke(args)
            print("ESP32_SOFTAP_CONTROLLER_SCAN_RESTORE_PASS")

    print("ESP32_SOFTAP_CONTROLLER_STOPPED")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
