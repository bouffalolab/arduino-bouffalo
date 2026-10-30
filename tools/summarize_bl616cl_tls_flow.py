#!/usr/bin/env python3
"""Decode the optional layout-1 TLS flow suffix from resource-gate logs."""
import argparse
import json
from pathlib import Path
import re

FLOW_FIELDS = ('layout', 'matched', 'local_port', 'state', 'rcv_nxt', 'rcv_wnd',
               'rcv_ann_wnd', 'rcv_ann_right_edge', 'lastack', 'snd_nxt',
               'ooseq_bytes', 'refused_bytes', 'flags', 'nrtx', 'rtime',
               'tls_readable', 'peeked', 'tuple_valid')


def decode(text):
    records = []
    slot = None
    for line in text.splitlines():
        identified = re.fullmatch(r'RA4_TLS_RESOURCE:RX_BURST_INCOMPLETE_SLOT=(\d+)', line)
        if identified:
            slot = int(identified[1])
        match = re.fullmatch(r'RA4_TLS_RESOURCE:(RX_BURST_(?:PENDING|INCOMPLETE|FAILURE))_DIAG=([-0-9,]+)', line)
        if not match:
            continue
        values = [int(v) for v in match[2].split(',')]
        if len(values) == 44:  # Older/default-off firmware, no flow suffix.
            continue
        if len(values) != 44 + len(FLOW_FIELDS) or values[44] != 1:
            raise ValueError('unsupported TLS flow diagnostic layout')
        flow = dict(zip(FLOW_FIELDS, values[44:]))
        if flow['rtime'] >= 2**31:
            flow['rtime'] -= 2**32
        records.append(dict(label=match[1], tls_slot=slot if match[1].endswith('INCOMPLETE') else 2000,
                            phase=values[0], error=values[1], flow=flow))
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    args = parser.parse_args()
    print(json.dumps(decode(args.log.read_text(errors='replace')), indent=2))


if __name__ == '__main__':
    main()
