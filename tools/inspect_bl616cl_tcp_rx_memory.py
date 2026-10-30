#!/usr/bin/env python3
"""Read bounded native RX ownership counters without addresses or payloads."""
import argparse
import json
import os
from pathlib import Path
import time
from inspect_bl616cl_gpio35 import find_hidraw, read_feature, select_feature_page, FEATURE_VERSION

FIELDS = ('sequence_used', 'physical_used', 'future_reserved', 'minimum_reserved',
          'live_flows', 'retired_flows', 'blocks', 'faults', 'admitted', 'denied',
          'physical_peak', 'block_peak', 'type_rejected')


def decode(report):
    if len(report) != 64 or report[:8] != b'U35D' + bytes((FEATURE_VERSION, 59, 1, 1)):
        raise ValueError('native RX owner diagnostic unavailable or disabled')
    result = dict(zip(FIELDS, (int.from_bytes(report[i:i+4], 'little') for i in range(8, 60, 4))))
    if (result['sequence_used'] + result['minimum_reserved'] > 12288 or
            not result['physical_used'] <= result['physical_peak'] <= 20480 or
            not result['blocks'] <= result['block_peak'] <= 128 or
            result['live_flows'] + result['retired_flows'] > 32 or result['faults']):
        raise ValueError('invalid native RX ownership accounting')
    return result


PHYSICAL_FIELDS = ('physical_used', 'physical_reserved', 'blocks', 'blocks_reserved',
                   'max_owner_charge', 'max_owner_blocks', 'owner_minimum', 'owner_cap',
                   'owner_block_cap', 'physical_limit', 'block_limit', 'faults')

def decode_physical(report):
    if len(report) != 64 or report[:8] != b'U35D' + bytes((FEATURE_VERSION, 60, 1, 1)):
        raise ValueError('physical RX fairness diagnostic unavailable or disabled')
    r = dict(zip(PHYSICAL_FIELDS, (int.from_bytes(report[i:i+4], 'little') for i in range(8, 56, 4))))
    # The 512 B first candidate is retained for strict historical decoding.
    if (r['owner_minimum'] not in (256, 512) or
            (r['owner_cap'], r['owner_block_cap'], r['physical_limit'], r['block_limit'])
            != (4096, 16, 20480, 128) or
            r['physical_used'] + r['physical_reserved'] > r['physical_limit'] or
            r['blocks'] + r['blocks_reserved'] > r['block_limit'] or
            r['max_owner_charge'] > r['owner_cap'] or r['max_owner_blocks'] > r['owner_block_cap'] or
            r['max_owner_charge'] > r['physical_used'] or r['max_owner_blocks'] > r['blocks'] or r['faults']):
        raise ValueError('invalid physical RX fairness accounting')
    return r


def capture(physical=False):
    fd = os.open(find_hidraw(), os.O_RDWR | os.O_NONBLOCK)
    try:
        select_feature_page(fd, 60 if physical else 59)
        return (decode_physical if physical else decode)(read_feature(fd))
    finally:
        try:
            select_feature_page(fd, 0)
        finally:
            os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gate-log', type=Path)
    parser.add_argument('--seconds', type=int, default=900)
    parser.add_argument('--physical', action='store_true')
    args = parser.parse_args()
    deadline = time.monotonic() + args.seconds
    previous = None
    while True:
        log = args.gate_log.read_text(errors='replace') if args.gate_log and args.gate_log.exists() else ''
        try:
            result = capture(args.physical)
        except (OSError, RuntimeError):
            # AP association deliberately detaches USB before pressure. This
            # does not permit losing a sample transport during pressure itself.
            if args.gate_log is None or ',PRESSURE_START_REQUEST' in log:
                raise
            print('TCP_RX_MEMORY_PREPARATION_USB_UNAVAILABLE', flush=True)
        else:
            if result != previous:
                print(('TCP_RX_PHYSICAL=' if args.physical else 'TCP_RX_MEMORY=') + json.dumps(result, sort_keys=True), flush=True)
                previous = result
        if args.gate_log is None:
            break
        if 'RA4_TLS_RESOURCE:RESULT=' in log or '\nERROR:' in log:
            print('TCP_RX_MEMORY_SAMPLER_COMPLETE', flush=True)
            break
        if time.monotonic() >= deadline:
            raise RuntimeError('native RX ownership sampling time bound reached')
        time.sleep(2)


if __name__ == '__main__':
    main()
