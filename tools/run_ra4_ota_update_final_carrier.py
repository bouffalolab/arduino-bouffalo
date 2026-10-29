#!/usr/bin/env python3
"""Validate the BL616CL-owned RA4M1 OTA cache-to-Flash write path.

This gate deliberately separates host BOSSA from the feature under test:
host BOSSA only installs a temporary RA4M1 AT proxy and restores the normal
WiFiS3 scan Sketch. The marker Sketch is downloaded as an Arduino OTA
container, committed by BL616CL LittleFS, and written by BL616CL's internal
Ra4SambaProgrammer through +OTAUPDATE.
"""

from __future__ import annotations

import argparse
import ipaddress
import os
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import serial

from inspect_bl616cl_gpio35 import (
    FEATURE_VERSION,
    find_hidraw,
    read_feature,
    select_feature_page,
)
from run_bl616cl_apsta_gate import DEFAULT_CREDENTIAL_ENV, load_private_credentials, quote_at_argument
import run_ra4_ota_download_final_carrier as ota_download
import run_ra4_wifis3_scan_final_carrier as final_scan
from uno_r4_usb_identity import resolve_paired_bl616cl_port


ROOT_DIR = Path(__file__).resolve().parents[1]
FQBN = "arduino:renesas_uno:unor4wifi"
MARKER_SKETCH = ROOT_DIR / "examples" / "ra4-ota-update-marker"
MARKER_BUILD = ROOT_DIR / "build" / "ra4-ota-update-marker-final-carrier"
MARKER_IMAGE = MARKER_BUILD / "ra4-ota-update-marker.ino.bin"
WORK_ROOT = ROOT_DIR / "build" / "ra4-ota-update-final-carrier"
OTA_PATH = "/ra4-ota-update-marker.bin"
OTA_PORT = ota_download.OTA_PORT
MARKER = b"RA4_OTA_UPDATE_MARKER:BOOT"
PROGRAMMER_PAGE = 35


def configure_softap_network(bridge, response_timeout: float) -> None:
    # WiFiS3 setDNS() also writes SOFTAPCONFIG. A previous STA fixture can
    # therefore leave a valid AP configuration outside this gate's fixed subnet.
    ota_download.require_exact(
        ota_download.transact(bridge,
            b"AT+SOFTAPCONFIG=192.168.4.1,192.168.4.1,255.255.255.0\r\n",
            response_timeout),
        b"+SOFTAPCONFIG: \r\nOK\r\n", "OTA test SOFTAPCONFIG")


def verify_softap_network(bridge, response_timeout: float) -> None:
    response = ota_download.transact(bridge, b"AT+IPSOFTAP\r\n", response_timeout)
    if response != b"+IPSOFTAP: 192.168.4.1\r\nOK\r\n":
        # Do not print a retained private STA address on failure.
        raise RuntimeError("OTA SoftAP address differs from configured test subnet")
    print("RA4_OTA_UPDATE_SOFTAP_ADDRESS_PASS", flush=True)


def capture_association_snapshot(bridge, response_timeout: float, phase: str) -> None:
    """Read numeric radio state before cleanup can remove the failed AP."""
    try:
        response = ota_download.transact(bridge, b"AT+COEXDIAG?\r\n", response_timeout)
        match = re.fullmatch(rb"\+COEXDIAG: ([0-9]+(?:,[0-9]+){35})\r\nOK\r\n", response)
        if match is None:
            print(f"RA4_OTA_UPDATE_ASSOCIATION_{phase}_COEX_UNAVAILABLE", flush=True)
        else:
            print(f"RA4_OTA_UPDATE_ASSOCIATION_{phase}_COEX=" + match[1].decode("ascii"), flush=True)
    except (OSError, RuntimeError, serial.SerialException):
        print(f"RA4_OTA_UPDATE_ASSOCIATION_{phase}_COEX_UNAVAILABLE", flush=True)
    for option in ("--ap-start", "--sta-disconnect", "--runtime-heap"):
        try:
            result = subprocess.run(
                [sys.executable, str(ROOT_DIR / "tools/inspect_bl616cl_gpio35.py"), option],
                capture_output=True, text=True, errors="replace", timeout=8,
            )
            print(f"RA4_OTA_UPDATE_ASSOCIATION_{phase}_HID={option},rc:{result.returncode}", flush=True)
            print(result.stdout, end="", flush=True)
        except (OSError, subprocess.SubprocessError):
            print(f"RA4_OTA_UPDATE_ASSOCIATION_{phase}_HID_UNAVAILABLE={option}", flush=True)
    fd = -1
    try:
        fd = os.open(find_hidraw(), os.O_RDWR | os.O_NONBLOCK)
        for page, fields in (
            (27, (("input", 6, 4), ("sta", 10, 4), ("ap", 14, 4),
                  ("ap_ipv4", 18, 4), ("malformed", 30, 4))),
            (29, (("ap_udp", 6, 4), ("rx_source_port", 18, 2),
                  ("rx_destination_port", 20, 2), ("rx_is_sta", 30, 1),
                  ("tx_source_port", 31, 2), ("tx_destination_port", 33, 2),
                  ("tx_is_sta", 35, 1))),
        ):
            select_feature_page(fd, page)
            report = read_feature(fd)
            if report[:6] != b"U35D" + bytes((FEATURE_VERSION, page)):
                raise RuntimeError("packet snapshot unavailable")
            values = ",".join(f"{name}:{int.from_bytes(report[offset:offset + size], 'little')}"
                              for name, offset, size in fields)
            print(f"RA4_OTA_UPDATE_ASSOCIATION_{phase}_PACKETS={page}," + values, flush=True)
            if page == 29 and report[36] in (1, 2):
                print(f"RA4_OTA_UPDATE_ASSOCIATION_{phase}_DHCP=" + ",".join(
                    f"{name}:{int.from_bytes(report[offset:offset + 4], 'little')}"
                    for name, offset in (("ap_client_input", 40), ("sta_server_input", 44),
                                         ("server_output_all_vifs", 48))), flush=True)
                if report[36] == 2:
                    print(f"RA4_OTA_UPDATE_ASSOCIATION_{phase}_DHCP_TYPES=" + ",".join(
                        f"{name}:{int.from_bytes(report[offset:offset + 4], 'little')}"
                        for name, offset in (("offer", 52), ("ack", 56), ("nak", 60))), flush=True)
        select_feature_page(fd, 52)
        report = read_feature(fd)
        if len(report) == 64 and report[:6] == b"U35D" + bytes((FEATURE_VERSION, 52)) and report[7] == 1:
            fields = [("enabled", report[6])]
            for i, message in enumerate(("offer", "ack", "nak")):
                for j, counter in enumerate(("released", "discarded", "acknowledged")):
                    offset = 8 + 12*i + 4*j
                    fields.append((message + "_" + counter,
                                   int.from_bytes(report[offset:offset+4], "little")))
            print(f"RA4_OTA_UPDATE_ASSOCIATION_{phase}_DHCP_TX=" +
                  ",".join(f"{name}:{value}" for name, value in fields), flush=True)
    except (OSError, RuntimeError):
        print(f"RA4_OTA_UPDATE_ASSOCIATION_{phase}_PACKETS_UNAVAILABLE", flush=True)
    finally:
        if fd >= 0:
            try:
                select_feature_page(fd, 0)
            except (OSError, RuntimeError):
                print(f"RA4_OTA_UPDATE_ASSOCIATION_{phase}_PACKETS_RESTORE_UNAVAILABLE", flush=True)
            finally:
                os.close(fd)


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def compile_marker(args: argparse.Namespace) -> Path:
    run_command(
        [
            args.arduino_cli,
            "compile",
            "--fqbn",
            FQBN,
            "--clean",
            "--build-path",
            str(MARKER_BUILD),
            str(MARKER_SKETCH),
        ]
    )
    if not MARKER_IMAGE.is_file() or MARKER_IMAGE.stat().st_size == 0:
        raise RuntimeError("compiled RA4M1 OTA marker image is missing or empty")
    return MARKER_IMAGE


def make_literal_ota(decoded: bytes) -> bytes:
    if not decoded:
        raise RuntimeError("RA4M1 OTA marker image is empty")

    compressed = bytearray()
    bits = 0
    bit_count = 0
    for value in decoded:
        bits = (bits << 9) | 0x100 | value
        bit_count += 9
        while bit_count >= 8:
            bit_count -= 8
            compressed.append((bits >> bit_count) & 0xFF)
            bits &= (1 << bit_count) - 1
    if bit_count != 0:
        compressed.append((bits << (8 - bit_count)) & 0xFF)

    header = bytearray(20)
    header[0:4] = (12 + len(compressed)).to_bytes(4, "little")
    header[8:12] = ota_download.OTA_MAGIC.to_bytes(4, "little")
    crc = ota_download.binascii.crc32(header[8:20] + compressed) & 0xFFFFFFFF
    header[4:8] = crc.to_bytes(4, "little")
    return bytes(header + compressed)


def require_programmer_report(report: bytes) -> None:
    if report[:5] != b"U35D" + bytes((FEATURE_VERSION,)):
        raise RuntimeError("unexpected BL616CL diagnostic feature signature")
    if report[5] != PROGRAMMER_PAGE:
        raise RuntimeError("unexpected RA4 SAM-BA programmer diagnostic page")


def read_programmer_report(fd: int) -> bytes:
    select_feature_page(fd, PROGRAMMER_PAGE)
    report = read_feature(fd)
    require_programmer_report(report)
    return report


def read_u32_le(report: bytes, offset: int) -> int:
    return int.from_bytes(report[offset : offset + 4], "little")


def programmer_snapshot(report: bytes) -> tuple[int, int, int, int, int, int, int]:
    return (
        read_u32_le(report, 10),
        report[6],
        report[7],
        report[8],
        report[9],
        read_u32_le(report, 34),
        int.from_bytes(report[38:40], "little"),
    )


def wait_for_programmer(
    fd: int,
    initial_sequence: int,
    expected_image_bytes: int,
    timeout_seconds: float,
    interrupt_after_bytes: int = 0,
) -> bool:
    deadline = time.monotonic() + timeout_seconds
    last_snapshot: tuple[int, int, int, int, int, int, int] | None = None
    while time.monotonic() < deadline:
        report = read_programmer_report(fd)
        snapshot = programmer_snapshot(report)
        if snapshot != last_snapshot:
            print(
                "RA4_OTA_PROGRAM_PROGRESS="
                f"sequence={snapshot[0]} state={snapshot[1]} error={snapshot[2]} "
                f"active={snapshot[3]} succeeded={snapshot[4]} "
                f"verified_bytes={snapshot[5]} chunks={snapshot[6]}"
            )
            last_snapshot = snapshot
        if (interrupt_after_bytes and snapshot[0] > initial_sequence
                and snapshot[3] == 1 and snapshot[5] >= interrupt_after_bytes):
            print(f"RA4_OTA_PROGRAM_INTERRUPT_ARMED={snapshot[5]}", flush=True)
            return False
        if snapshot[0] > initial_sequence and snapshot[3] == 0:
            if snapshot[4] != 1 or snapshot[2] != 0:
                raise RuntimeError(
                    "BL616CL Ra4SambaProgrammer failed: "
                    f"state={snapshot[1]} error={snapshot[2]}"
                )
            image_bytes = read_u32_le(report, 30)
            verified_bytes = snapshot[5]
            expected_chunks = (expected_image_bytes + 4095) // 4096
            if (
                image_bytes != expected_image_bytes
                or verified_bytes != expected_image_bytes
                or snapshot[6] != expected_chunks
            ):
                raise RuntimeError(
                    "BL616CL programmer accounting mismatch: "
                    f"image={image_bytes} verified={verified_bytes} "
                    f"chunks={snapshot[6]} expected_image={expected_image_bytes} "
                    f"expected_chunks={expected_chunks}"
                )
            print(
                "RA4_OTA_PROGRAMMER_PASS="
                f"image_bytes={image_bytes} verified_bytes={verified_bytes} "
                f"chunks={snapshot[6]} tx={read_u32_le(report, 22)} "
                f"rx={read_u32_le(report, 26)}"
            )
            if interrupt_after_bytes:
                raise RuntimeError("program completed before requested interruption")
            return True
        time.sleep(0.05)
    raise RuntimeError("BL616CL Ra4SambaProgrammer did not complete before timeout")


def capture_marker(link: serial.Serial, timeout_seconds: float) -> None:
    deadline = time.monotonic() + timeout_seconds
    captured = bytearray()
    while time.monotonic() < deadline:
        chunk = link.read(256)
        if not chunk:
            continue
        captured.extend(chunk)
        if MARKER in captured:
            print("RA4_OTA_UPDATE_RUNTIME_MARKER_PASS")
            return
        if len(captured) > 4096:
            raise RuntimeError("RA4M1 OTA marker capture exceeded 4096 bytes")
    raise RuntimeError("RA4M1 OTA marker did not appear after BL616CL programming")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--credential-env", type=Path, default=DEFAULT_CREDENTIAL_ENV)
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--response-timeout-ms", type=int, default=2000)
    parser.add_argument("--sta-timeout-ms", type=int, default=45000)
    parser.add_argument("--download-timeout-ms", type=int, default=45000)
    parser.add_argument("--program-timeout-ms", type=int, default=60000)
    parser.add_argument("--runtime-timeout-ms", type=int, default=10000)
    parser.add_argument("--runtime-settle-ms", type=int, default=1000)
    parser.add_argument("--peer-ready-timeout-ms", type=int, default=10000)
    parser.add_argument("--peer-chunk-bytes", type=int, default=512)
    parser.add_argument("--peer-chunk-delay-ms", type=int, default=15)
    parser.add_argument("--tcp-diagnostics", action="store_true",
                        help="Collect read-only Windows TCP counters during download.")
    parser.add_argument("--peer-tcp-nodelay", action="store_true",
                        help="Diagnostic: disable peer Nagle coalescing without changing workload.")
    parser.add_argument("--source-only", action="store_true")
    parser.add_argument("--interrupt-after-bytes", type=int, default=0,
                        help="Reset BL616CL during active internal programming, then prove host scan recovery.")
    parser.add_argument("--softap", action="store_true",
                        help="Download from Windows over a temporary board AP without an upstream AP.")
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    values = (
        args.response_timeout_ms,
        args.sta_timeout_ms,
        args.download_timeout_ms,
        args.program_timeout_ms,
        args.runtime_timeout_ms,
        args.peer_ready_timeout_ms,
        args.peer_chunk_bytes,
    )
    if args.interrupt_after_bytes < 0 or any(value <= 0 for value in values) or (
        args.runtime_settle_ms < 0 or args.peer_chunk_delay_ms < 0
    ):
        raise RuntimeError("invalid RA4 OTA update gate argument")

    marker_image = compile_marker(args)
    marker_bytes = marker_image.read_bytes()
    if len(marker_bytes) > 240 * 1024:
        raise RuntimeError("RA4M1 OTA marker image exceeds application Flash")
    package = make_literal_ota(marker_bytes)
    if args.source_only:
        ota_download.compile_proxy(args)
        print(f"RA4_OTA_UPDATE_MARKER_IMAGE_BYTES={len(marker_bytes)}")
        print(f"RA4_OTA_UPDATE_PACKAGE_BYTES={len(package)}")
        print("RA4_OTA_UPDATE_SOURCE_GATE_PASS")
        return 0

    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    final_scan.require_final_carrier_bridge(args.bridge_port)
    if args.softap:
        ssid, password = "uno-r4-" + secrets.token_hex(4), secrets.token_urlsafe(18)
        host = ipaddress.IPv4Address("192.168.4.100")
    else:
        ssid, password = load_private_credentials(args.credential_env)
        host = ota_download.windows_wlan_address()
    response_timeout = args.response_timeout_ms / 1000.0
    WORK_ROOT.mkdir(parents=True, exist_ok=True)
    work_dir = Path(tempfile.mkdtemp(prefix="ra4-ota-update-", dir=WORK_ROOT))
    package_path = work_dir / "ra4.ota"
    package_path.write_bytes(package)

    print(f"BL616CL final carrier: {args.bridge_port} (2341:1002 interface 00)")
    print("Target: BL616CL committed OTA cache -> internal SAM-BA -> RA4M1 application Flash.")
    print("Recovery: the normal RA4M1 WiFiS3 scan Sketch is restored after this gate.")
    print("Privacy: STA credentials and raw AT traffic are not printed or retained.")

    bridge: serial.Serial | None = None
    peer: subprocess.Popen[str] | None = None
    hid_fd = -1
    proxy_uploaded = False
    sta_connected = False
    primary_error: Exception | None = None
    peer_output = ""
    softap_session = None
    prepared_reset = None
    try:
        proxy_image = ota_download.compile_proxy(args)
        if args.interrupt_after_bytes:
            from prepared_bl616cl_reset import PreparedBlReset
            prepared_reset = PreparedBlReset(work_dir)
        ota_download.upload_proxy(args, proxy_image)
        proxy_uploaded = True
        bridge = ota_download.open_bridge(args.bridge_port)
        bridge.reset_input_buffer()

        ota_download.require_exact(
            ota_download.transact(bridge, b"AT+WIFIMODE=2\r\n" if args.softap else b"AT+WIFIMODE=1\r\n", response_timeout),
            b"+WIFIMODE: \r\nOK\r\n",
            "WIFIMODE=1",
        )
        ota_download.require_exact(
            ota_download.transact(bridge, b"AT+RECONNECT=0\r\n", response_timeout),
            b"OK\r\n",
            "RECONNECT=0",
        )
        if args.softap:
            from bl616cl_softap_test_session import SoftApTestSession
            configure_softap_network(bridge, response_timeout)
            softap_session = SoftApTestSession(work_dir)
            softap_session.prepare_association(args, ssid, password, "192.168.4.1")
            command = (f"AT+BEGINSOFTAP={quote_at_argument(ssid)},"
                       f"{quote_at_argument(password)},1\r\n").encode("ascii")
            ota_download.require_exact(ota_download.transact(bridge, command, response_timeout),
                                       b"+BEGINSOFTAP: 1\r\nOK\r\n", "BEGINSOFTAP")
            sta_connected = True  # Clean up an acknowledged AP even if ready times out.
            deadline = time.monotonic() + args.sta_timeout_ms / 1000.0
            while ota_download.parse_status(ota_download.transact(
                    bridge, b"AT+GETSTATUS?\r\n", response_timeout)) not in (7, 8):
                if time.monotonic() >= deadline:
                    raise RuntimeError("OTA SoftAP did not become ready")
                time.sleep(0.25)
            verify_softap_network(bridge, response_timeout)
            capture_association_snapshot(bridge, response_timeout, "BEFORE")
            try:
                softap_session.connect_prepared()
            except (OSError, RuntimeError, subprocess.SubprocessError):
                capture_association_snapshot(bridge, response_timeout, "FAILED")
                raise
            capture_association_snapshot(bridge, response_timeout, "CONNECTED")
            if ipaddress.IPv4Address(softap_session.current_address()) != host:
                raise RuntimeError("OTA SoftAP peer DHCP address differs from listener")
            print("RA4_OTA_UPDATE_SOFTAP_CONNECT_PASS")
        else:
            begin_sta = (
                "AT+BEGINSTA="
                + quote_at_argument(ssid)
                + ("," + quote_at_argument(password) if password else "")
                + "\r\n"
            ).encode("ascii")
            response = ota_download.transact(bridge, begin_sta, response_timeout)
            if not response.startswith(b"+BEGINSTA: ") or not response.endswith(b"OK\r\n"):
                raise RuntimeError("BEGINSTA did not return the expected AT framing")
            ota_download.wait_for_sta(bridge, args.sta_timeout_ms / 1000.0, response_timeout)
            sta_connected = True
            print("RA4_OTA_UPDATE_STA_CONNECT_PASS")
        # Keep the peer in its own directory: cleanup must not kill the WLAN
        # owner before it deletes the temporary profile and restores the host.
        peer_dir = work_dir / "peer"
        peer_dir.mkdir()
        peer = ota_download.launch_windows_peer(peer_dir, host, package_path,
                                               args.peer_chunk_bytes, args.peer_chunk_delay_ms, False,
                                               tcp_diagnostics=args.tcp_diagnostics,
                                               tcp_nodelay=args.peer_tcp_nodelay)
        peer_output = ota_download.wait_for_peer_ready(
            peer, "HTTP_OTA_PEER", args.peer_ready_timeout_ms / 1000.0)

        download_request = (
            f"AT+OTADOWNLOAD=http://{host}:{OTA_PORT}/ra4.ota,{OTA_PATH}\r\n"
        ).encode("ascii")
        download_response = ota_download.transact_synchronous_download_with_telemetry(
            bridge,
            download_request,
            args.download_timeout_ms / 1000.0,
            tcp_diagnostics=args.tcp_diagnostics,
        )
        ota_download.require_exact(
            download_response,
            f"+OTADOWNLOAD: {len(marker_bytes)}\r\nOK\r\n".encode("ascii"),
            "OTADOWNLOAD marker image",
        )
        ota_download.require_exact(
            ota_download.transact(bridge, b"AT+OTAVERIFY\r\n", response_timeout),
            b"+OTAVERIFY: 0\r\nOK\r\n",
            "OTAVERIFY marker image",
        )
        print("RA4_OTA_UPDATE_CACHE_COMMIT_PASS")

        ota_download.require_exact(
            ota_download.transact(bridge, b"AT+DISCONNECT\r\n", response_timeout),
            b"+DISCONNECT: \r\nOK\r\n",
            "DISCONNECT before programming",
        )
        sta_connected = False

        if args.interrupt_after_bytes:
            ota_download.require_exact(
                ota_download.transact(bridge,b"AT+OTAVERIFY\r\n",response_timeout),
                b"+OTAVERIFY: 0\r\nOK\r\n","cache survived reset preparation")

        hidraw = find_hidraw()
        hid_fd = os.open(hidraw, os.O_RDWR | os.O_NONBLOCK)
        before = read_programmer_report(hid_fd)
        initial_sequence = read_u32_le(before, 10)

        bridge.reset_input_buffer()
        command = f"AT+OTAUPDATE={OTA_PATH}\r\n".encode("ascii")
        if bridge.write(command) != len(command):
            raise RuntimeError("OTAUPDATE request write was incomplete")
        bridge.flush()
        print("RA4_OTA_UPDATE_PROGRAM_REQUESTED")
        completed = wait_for_programmer(
            hid_fd,
            initial_sequence,
            len(marker_bytes),
            args.program_timeout_ms / 1000.0,
            args.interrupt_after_bytes,
        )
        if completed:
            bridge.baudrate = 115200
            bridge.reset_input_buffer()
            time.sleep(args.runtime_settle_ms / 1000.0)
            capture_marker(bridge, args.runtime_timeout_ms / 1000.0)
        else:
            # No host upload may occur until the active programmer has really
            # been interrupted. The finally block subsequently restores scan.
            select_feature_page(hid_fd, 0)
            os.close(hid_fd)
            hid_fd = -1
            bridge.close()
            bridge = None
            prepared_reset.fire()
            args.bridge_port = resolve_paired_bl616cl_port("auto")
            final_scan.require_final_carrier_bridge(args.bridge_port)
            print("RA4_OTA_ACTIVE_PROGRAM_BRIDGE_RESET_PASS", flush=True)

        if peer.poll() is None:
            peer.wait(timeout=5)
        if peer.stdout is not None:
            peer_output += peer.stdout.read()
        if "HTTP_OTA_PEER:REQUEST" not in peer_output:
            raise RuntimeError("HTTP peer did not observe the OTA marker request")
        if "HTTP_OTA_PEER:PAYLOAD_SENT" not in peer_output:
            raise RuntimeError("HTTP peer did not complete the OTA marker stream")
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as error:
        primary_error = error
    finally:
        if prepared_reset is not None:
            try:
                prepared_reset.close()
            except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                if primary_error is None:
                    primary_error=error
        if hid_fd >= 0:
            try:
                select_feature_page(hid_fd, 0)
            except OSError:
                pass
            os.close(hid_fd)
        if bridge is not None and bridge.is_open:
            if sta_connected:
                try:
                    ota_download.require_exact(
                        ota_download.transact(
                            bridge,
                            b"AT+DISCONNECT\r\n",
                            response_timeout,
                        ),
                        b"+DISCONNECT: \r\nOK\r\n",
                        "cleanup DISCONNECT",
                    )
                except (OSError, RuntimeError, serial.SerialException):
                    print("RA4_OTA_UPDATE_CLEANUP_STA_DISCONNECT_FAILED", file=sys.stderr)
            bridge.close()
        if peer is not None:
            ota_download.stop_windows_peer(peer, peer_dir)
            if peer.stdout is not None:
                peer_output += peer.stdout.read()
        if softap_session is not None:
            try:
                softap_session.close()
            except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                if primary_error is None:
                    primary_error = error
        if proxy_uploaded:
            try:
                ota_download.restore_scan(args)
            except (
                OSError,
                RuntimeError,
                serial.SerialException,
                subprocess.SubprocessError,
            ) as error:
                print("FINAL_CARRIER_RA4_SCAN_RESTORE_FAILED", file=sys.stderr)
                if primary_error is None:
                    primary_error = error
        for line in peer_output.splitlines():
            if line.startswith(("HTTP_OTA_PEER:", "HTTPS_OTA_PEER:")):
                print(line)
        if primary_error is None:
            shutil.rmtree(work_dir, ignore_errors=True)
        else:
            print(f"RA4_OTA_UPDATE_FAILURE_EVIDENCE={work_dir}", file=sys.stderr)

    if primary_error is not None:
        raise primary_error
    print("RA4_OTA_INTERRUPTED_PROGRAM_HOST_RECOVERY_PASS" if args.interrupt_after_bytes
          else "RA4_OTA_UPDATE_FINAL_CARRIER_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
