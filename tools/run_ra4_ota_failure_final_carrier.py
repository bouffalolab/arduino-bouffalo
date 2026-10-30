#!/usr/bin/env python3
"""Exercise malformed, truncated, oversized and reset OTA caches on the paired board.

Only the dedicated RA4M1 proxy/scan Sketches are uploaded. Generated arbitrary
OTA fixtures are never programmed: every OTAUPDATE here must be rejected.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import tempfile
import time

import run_ra4_ota_download_final_carrier as d
import run_ra4_ota_update_final_carrier as u
from run_bl616cl_generic_at_final_carrier import restore_usb
from uno_r4_usb_identity import resolve_paired_bl616cl_port


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("--softap",action="store_true");options=parser.parse_args()
    args = argparse.Namespace(bridge_port="auto", arduino_cli="arduino-cli", runtime_settle_ms=1000)
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    d.final_scan.require_final_carrier_bridge(args.bridge_port)
    args.softap=options.softap
    ssid, password = (None,None) if args.softap else d.load_private_credentials(d.DEFAULT_CREDENTIAL_ENV)
    root = d.ROOT_DIR / "build/ra4-ota-failure-final-carrier"
    root.mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="cases-", dir=root))
    network=None
    if args.softap:
        from ota_softap_test_network import OtaSoftApNetwork
        network=OtaSoftApNetwork(args,work)
    host=network.host if network else d.windows_wlan_address()
    bridge = None
    installed = False
    original_peer = d.WINDOWS_OTA_PEER

    def exact(request, expected, label):
        d.require_exact(d.transact(bridge, request, 3), expected, label)

    def connect():
        if network:
            network.start(bridge,3);return
        exact(b"AT+WIFIMODE=1\r\n", b"+WIFIMODE: \r\nOK\r\n", "mode")
        exact(b"AT+RECONNECT=0\r\n", b"OK\r\n", "reconnect")
        request = ("AT+BEGINSTA=" + d.quote_at_argument(ssid)
                   + ("," + d.quote_at_argument(password) if password else "") + "\r\n").encode()
        response = d.transact(bridge, request, 3)
        if re.fullmatch(rb"\+BEGINSTA: [0-9]+\r\nOK\r\n", response) is None:
            raise RuntimeError("STA request failed")
        d.wait_for_sta(bridge, 45, 3)

    def reject_update(label):
        fd = os.open(u.find_hidraw(), os.O_RDWR | os.O_NONBLOCK)
        try:
            before = u.programmer_snapshot(u.read_programmer_report(fd))[0]
            exact(b"AT+OTAUPDATE=/fault.bin\r\n", b"ERROR\r\n", label + " update rejection")
            after = u.programmer_snapshot(u.read_programmer_report(fd))[0]
            if after != before:
                raise RuntimeError("invalid cache entered Flash programmer")
        finally:
            u.select_feature_page(fd, 0)
            os.close(fd)

    def download(label, package, expected, truncated=False):
        directory = work / label
        directory.mkdir()
        path = directory / "ra4.ota"
        path.write_bytes(package[:-97] if truncated else package)
        d.WINDOWS_OTA_PEER = original_peer
        if truncated:
            d.WINDOWS_OTA_PEER = original_peer.replace(
                'str(len(package)).encode("ascii")',
                f'str({len(package)}).encode("ascii")')
        # Match the established successful OTA gate pacing. Peer stress is a
        # separate network workload, not part of these cache failure fixtures.
        peer = d.launch_windows_peer(directory, host, path, 512, 15, False)
        try:
            d.wait_for_peer_ready(peer, "HTTP_OTA_PEER", 15)
            print("RA4_OTA_FAILURE_CASE_BEGIN="+label,flush=True)
            response = d.transact(bridge,
                f"AT+OTADOWNLOAD=http://{host}:{d.OTA_PORT}/ra4.ota,/fault.bin\r\n".encode(), 100)
            match = re.fullmatch(rb"\+OTADOWNLOAD: (-?[0-9]+)\r\nOK\r\n", response)
            if match is None or int(match[1]) != expected:
                raise RuntimeError(f"{label}: unexpected OTA status " + (match[1].decode() if match else repr(response)))
            print(f"RA4_OTA_FAILURE_CASE_PASS={label},status={expected}", flush=True)
        finally:
            d.stop_windows_peer(peer, directory)
            d.WINDOWS_OTA_PEER = original_peer

    def reset_board():
        nonlocal bridge
        bridge.close()
        bridge = None
        restore_usb(physical_reset=True)
        if network:network.pause()
        args.bridge_port = resolve_paired_bl616cl_port("auto")
        d.final_scan.require_final_carrier_bridge(args.bridge_port)
        # The paired carrier can restart RA4M1 in SAM-BA when BL is reset.
        # Reinstall only the RA proxy; this does not touch BL Flash/cache.
        d.upload_proxy(args,image)
        print("RA4_PROXY_RESTART_AFTER_BRIDGE_RESET_PASS",flush=True)
        bridge = d.open_bridge(args.bridge_port)

    def interrupt_download():
        directory=work/"interrupted-download";directory.mkdir()
        payload=d.make_literal_ota(98304)
        path=directory/"ra4.ota";path.write_bytes(payload)
        peer=d.launch_windows_peer(directory,host,path,512,150,False)
        try:
            d.wait_for_peer_ready(peer,"HTTP_OTA_PEER",15)
            exact(f"AT+OTADOWNLOADSTART=http://{host}:{d.OTA_PORT}/ra4.ota,/fault.bin\r\n".encode(),
                  b"+OTADOWNLOADSTART: 0\r\nOK\r\n","async start")
            deadline=time.monotonic()+15
            while True:
                response=d.transact(bridge,b"AT+OTADOWNLOADPROGRESS\r\n",3)
                m=re.fullmatch(rb"\+OTADOWNLOADPROGRESS: ([0-9]+)\r\nOK\r\n",response)
                if not m: raise RuntimeError("active download status unavailable")
                if 4096 <= int(m[1]) < len(payload): break
                if time.monotonic()>deadline: raise RuntimeError("download did not become active")
                time.sleep(.1)
            print("RA4_OTA_DOWNLOAD_RESET_ARMED bytes="+m[1].decode(),flush=True)
            reset_board()
        finally:
            d.stop_windows_peer(peer,directory)
        reject_update("interrupted-download-reset")
        print("RA4_OTA_INTERRUPTED_DOWNLOAD_RESET_PASS",flush=True)

    try:
        image = d.compile_proxy(args)
        d.upload_proxy(args, image)
        installed = True
        bridge = d.open_bridge(args.bridge_port)
        connect()
        package = d.make_literal_ota(32768)
        bad_crc = bytearray(package); bad_crc[4] ^= 1
        bad_magic = bytearray(package); bad_magic[8] ^= 1
        for label, payload, expected, truncated in (
            ("crc", bytes(bad_crc), -10, False),
            ("magic", bytes(bad_magic), -11, False),
            ("truncated", package, -12, True),
            ("over-capacity", d.make_literal_ota(240*1024+1), -9, False),
        ):
            download(label, payload, expected, truncated)
            exact(b"AT+OTAVERIFY\r\n",f"+OTAVERIFY: {expected}\r\nOK\r\n".encode(),label+" verify rejection")
            reject_update(label)
            download(label + "-recovery", package, 32768)
            exact(b"AT+OTAVERIFY\r\n", b"+OTAVERIFY: 0\r\nOK\r\n", "recovered cache")

        # Prove the real maximum fits the independent partition, then prove a
        # BL reset cannot turn orphaned bytes into an authorized Flash image.
        download("maximum-240KiB", d.make_literal_ota(240*1024), 240*1024)
        reset_board()
        reject_update("committed-cache-reset")
        response = d.transact(bridge, b"AT+OTAVERIFY\r\n", 3)
        if response == b"+OTAVERIFY: 0\r\nOK\r\n":
            raise RuntimeError("reset cache incorrectly retained Ready state")
        connect()
        download("reset-redownload", package, 32768)
        exact(b"AT+OTAVERIFY\r\n", b"+OTAVERIFY: 0\r\nOK\r\n", "reset recovery")
        interrupt_download()
        connect()
        download("interrupted-redownload",package,32768)
        exact(b"AT+OTAVERIFY\r\n",b"+OTAVERIFY: 0\r\nOK\r\n","interruption recovery")
    finally:
        d.WINDOWS_OTA_PEER = original_peer
        if bridge is not None:
            try:
                d.transact(bridge, b"AT+DISCONNECT\r\n", 3)
            except (OSError,RuntimeError):
                print("RA4_OTA_FAILURE_DISCONNECT_CLEANUP_FAILED",flush=True)
            finally:
                bridge.close()
        try:
            if network:network.close()
        finally:
            if installed:d.restore_scan(args)
    shutil.rmtree(work)
    print("RA4_OTA_FAILURE_FINAL_CARRIER_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
