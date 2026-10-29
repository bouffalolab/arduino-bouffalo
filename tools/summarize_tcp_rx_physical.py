#!/usr/bin/env python3
"""Strictly validate physical RX samples; maxima are sampled, not lifetime peaks."""
import argparse
import json
from pathlib import Path

from inspect_bl616cl_tcp_rx_memory import PHYSICAL_FIELDS, FEATURE_VERSION, decode_physical


def summarize(path):
    log = Path(path).read_text()
    if 'TCP_RX_MEMORY_SAMPLER_COMPLETE' not in log or 'Traceback' in log:
        raise ValueError('physical sampler did not complete')
    rows = []
    for line in log.splitlines():
        if not line.startswith('TCP_RX_PHYSICAL='):
            continue
        row = json.loads(line.split('=', 1)[1])
        if set(row) != set(PHYSICAL_FIELDS) or any(
                type(v) is not int or not 0 <= v <= 0xffffffff for v in row.values()):
            raise ValueError('unknown or invalid physical sample')
        report = b'U35D' + bytes((FEATURE_VERSION, 60, 1, 1)) + b''.join(
            row[k].to_bytes(4, 'little') for k in PHYSICAL_FIELDS) + bytes(8)
        decode_physical(report)
        rows.append(row)
    if len(rows) < 2 or not any(r['physical_used'] for r in rows):
        raise ValueError('no active physical pressure samples')
    if len({r['owner_minimum'] for r in rows}) != 1:
        raise ValueError('physical policy changed during sampling')
    return dict(samples=len(rows), faults=0, owner_minimum=rows[0]['owner_minimum'],
                sampled_max_physical=max(r['physical_used'] for r in rows),
                sampled_max_owner_charge=max(r['max_owner_charge'] for r in rows),
                sampled_max_owner_blocks=max(r['max_owner_blocks'] for r in rows),
                sampled_max_used_and_reserved=max(r['physical_used']+r['physical_reserved'] for r in rows),
                sampled_max_blocks_and_reserved=max(r['blocks']+r['blocks_reserved'] for r in rows))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(args.log), indent=2))
