#!/usr/bin/env python3
"""Single-attempt RA4 uploads with an application-side full-image CRC32.

Uses the dedicated BL616CL CDC only. Replaces the RA4 Sketch, alternates
64-byte and 4096-byte host bursts with no interpacket delay, and restores the
WiFiS3 scan Sketch. --standard-pacing instead checks the normal 64-byte/10ms
uploader path. CRC covers the actual binary, including vectors and data.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import zlib

import serial
import inspect_bl616cl_gpio35 as diagnostic
from uno_r4_usb_identity import resolve_paired_bl616cl_port
from run_ra4_wifis3_scan_final_carrier import require_final_carrier_bridge

ROOT = Path(__file__).resolve().parents[1]


def uart_snapshot():
    fd = os.open(diagnostic.find_hidraw(), os.O_RDWR)
    try:
        diagnostic.select_feature_page(fd, 0)
        report = diagnostic.read_feature(fd)
        if report[:5] != b'U35D' + bytes((diagnostic.FEATURE_VERSION,)):
            raise RuntimeError('unexpected UART telemetry signature')
        return {name: diagnostic.read_u32_le(report, offset) for name, offset in
                [('received', 32), ('dropped', 36), ('forwarded', 40), ('tx_failed', 44)]}
    finally:
        os.close(fd)


def read_crc(port, size):
    with serial.Serial(port, 115200, timeout=.1, write_timeout=2) as link:
        link.dtr = False
        link.rts = False
        link.reset_input_buffer()
        link.write(f'CHECK {size}\n'.encode('ascii'))
        link.flush()
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            line = link.readline().decode('ascii', errors='replace').strip()
            match = re.fullmatch(r'FLASH_INTEGRITY:SIZE=(\d+) CRC32=([0-9A-F]+) PROBE=([0-9A-F]+)', line)
            if match and int(match[1]) == size:
                return int(match[2], 16)
            if line.startswith('FLASH_INTEGRITY:FAIL='):
                raise RuntimeError(line)
    raise RuntimeError('RA4 full-image CRC response deadline')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work-dir', type=Path, required=True)
    parser.add_argument('--arduino-cli', default='arduino-cli')
    parser.add_argument('--rounds', type=int, default=10)
    parser.add_argument('--standard-pacing', action='store_true',
                        help='Validate the standard 64-byte/10ms upload path with full-image CRC')
    args = parser.parse_args()
    if args.rounds <= 0:
        parser.error('rounds must be positive')
    args.work_dir.mkdir(parents=True, exist_ok=False)
    build = args.work_dir / 'ra4-build'
    subprocess.run([args.arduino_cli, 'compile', '--fqbn', 'arduino:renesas_uno:unor4wifi',
                    '--build-path', str(build), str(ROOT / 'examples/ra4-flash-integrity-smoke')], check=True)
    image = build / 'ra4-flash-integrity-smoke.ino.bin'
    binary = image.read_bytes()
    expected = zlib.crc32(binary)
    print('FLASH_INTEGRITY_INPUT=' + json.dumps(dict(size=len(binary), crc32=f'{expected:08x}',
          sha256=hashlib.sha256(binary).hexdigest())), flush=True)
    results = []
    modified = False
    try:
        for iteration in range(args.rounds):
            # Elevated native identity/open check first; never infer a target
            # from tty ordering or stale Windows bus IDs after a replug.
            code = ('import os,sys;sys.path.insert(0,' + repr(str(ROOT / 'tools')) + ');'
                    "from uno_r4_usb_identity import resolve_paired_bl616cl_port;"
                    "p=resolve_paired_bl616cl_port('auto');"
                    "f=os.open(p,os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK);os.close(f);"
                    "print('FLASH_INTEGRITY_ROOT_IDENTITY_OPEN_PASS')")
            subprocess.run(['wsl.exe', '-d', 'Ubuntu-22.04', '-u', 'root', '--',
                            'python3', '-c', code], check=True, timeout=45)
            port = resolve_paired_bl616cl_port('auto')
            require_final_carrier_bridge(port)
            subprocess.run([args.arduino_cli, 'board', 'list'], check=True, timeout=30)
            before = uart_snapshot()
            packet = 64 if args.standard_pacing or iteration % 2 == 0 else 4096
            delay = 10 if args.standard_pacing else 0
            modified = True
            print(f'FLASH_INTEGRITY_UPLOAD round={iteration + 1} packet_bytes={packet} attempts=1 delay_ms={delay}', flush=True)
            subprocess.run([sys.executable, '-u', str(ROOT / 'tools/run_ra4_bossac_final_carrier.py'),
                            '--bridge-port', port, '--image', str(image), '--attempts', '1',
                            '--payload-packet-bytes', str(packet), '--payload-interpacket-delay-ms', str(delay),
                            '--runtime-settle-ms', '1000'], check=True, timeout=180)
            actual = read_crc(port, len(binary))
            after = uart_snapshot()
            result = dict(round=iteration + 1, packet_bytes=packet, delay_ms=delay, expected_crc32=f'{expected:08x}',
                          actual_crc32=f'{actual:08x}', before=before, after=after)
            print('FLASH_INTEGRITY_RESULT=' + json.dumps(result, sort_keys=True), flush=True)
            if actual != expected or after['dropped'] != before['dropped']:
                raise RuntimeError('RA4 full-image CRC mismatch or CDC byte loss')
            results.append(result)
    finally:
        (args.work_dir / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
        if modified:
            subprocess.run([sys.executable, '-u', str(ROOT / 'tools/run_ra4_wifis3_scan_final_carrier.py'),
                            '--arduino-cli', args.arduino_cli], check=True, timeout=180)
    print(f'FLASH_INTEGRITY_GATE_PASS rounds={len(results)} retries=0', flush=True)


if __name__ == '__main__':
    main()
