#!/usr/bin/env python3
"""Read failure-only lwIP heap evidence; optional bounded sampling during a gate."""
import argparse
import json
import os
from pathlib import Path
import time
from inspect_bl616cl_gpio35 import find_hidraw, read_feature, select_feature_page, FEATURE_VERSION

def capture():
    fd = os.open(find_hidraw(), os.O_RDWR | os.O_NONBLOCK)
    try:
        records = []
        for page in range(53, 58):
            select_feature_page(fd, page)
            report = read_feature(fd)
            if report[:6] != b'U35D' + bytes((FEATURE_VERSION, page)) or report[7] != 1:
                raise RuntimeError('lwIP failure diagnostic page unavailable')
            if not report[6]:
                raise RuntimeError('lwIP failure diagnostic disabled')
            values = [int.from_bytes(report[n:n+4], 'little') for n in range(8, 48, 4)]
            if page == 53:
                total = values[0]
            elif values[0]:
                names = ('sequence', 'requested', 'aligned', 'free_payload', 'largest_hole',
                         'holes', 'walk_valid', 'charged_used', 'native_errors', 'caller_pc')
                records.append(dict(zip(names, values)))
        return dict(total=total, entries=sorted(records, key=lambda e: e['sequence']))
    finally:
        try:
            select_feature_page(fd, 0)
        finally:
            os.close(fd)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gate-log', type=Path)
    parser.add_argument('--seconds', type=int, default=720)
    args = parser.parse_args()
    deadline = time.monotonic() + args.seconds
    previous = None
    while True:
        result = capture()
        if result != previous:
            print('LWIP_ALLOCATION_FAILURE=' + json.dumps(result, sort_keys=True), flush=True)
            previous = result
        if args.gate_log is None:
            break
        log = args.gate_log.read_text(errors='replace') if args.gate_log.exists() else ''
        if 'FINAL_CARRIER_RA4_SCAN_RESTORE_PASS' in log or '\nERROR:' in log:
            print('LWIP_ALLOCATION_SAMPLER_COMPLETE', flush=True)
            break
        if time.monotonic() >= deadline:
            raise RuntimeError('allocation sampler time bound reached')
        time.sleep(2)

if __name__ == '__main__':
    main()
