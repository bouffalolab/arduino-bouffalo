#!/usr/bin/env python3
"""Decode read-only PCB credit and independent native heap samples."""
import argparse
import json
from pathlib import Path

FIELDS = ('pool_capacity', 'pool_used', 'active', 'established', 'other_active', 'bound', 'timewait',
          'cap_total', 'accept_total', 'window_debit_total', 'announced_outstanding_total',
          'announced_over_accept', 'min_accept', 'max_accept', 'max_announced_outstanding',
          'zero_windows', 'ooseq_segments', 'ooseq_pbufs', 'ooseq_bytes', 'refused_pbufs',
          'refused_bytes', 'heap_capacity', 'heap_used', 'heap_peak', 'heap_errors')
LABELS = ('BEFORE_OPEN', 'BEFORE_RX', 'AFTER_RX', 'AFTER_CLOSE')


def decode(text):
    parts = text.split(',')
    if len(parts) != 2 + len(FIELDS) or any(not p.isdecimal() for p in parts):
        raise ValueError('invalid TCP RX budget width or numeric field')
    values = list(map(int, parts))
    if values[:2] != [1, 1] or any(v > 0xffffffff for v in values):
        raise ValueError('disabled or unknown TCP RX budget layout')
    row = dict(zip(FIELDS, values[2:]))
    if not 0 < row['pool_capacity'] <= 65535 or row['pool_used'] > row['pool_capacity']:
        raise ValueError('invalid PCB pool count')
    if row['active'] + row['bound'] + row['timewait'] > row['pool_used']:
        raise ValueError('registered PCB count exceeds native pool usage')
    if row['established'] + row['other_active'] != row['active']:
        raise ValueError('invalid active state counts')
    if row['accept_total'] + row['window_debit_total'] != row['cap_total']:
        raise ValueError('invalid receive credit accounting')
    if (row['announced_over_accept'] or
            row['announced_outstanding_total'] > row['accept_total'] or
            row['zero_windows'] > row['active'] or
            row['min_accept'] > row['max_accept'] or
            row['max_announced_outstanding'] > row['max_accept']):
        raise ValueError('invalid receive window promise')
    if row['active']:
        if not (row['min_accept'] * row['active'] <= row['accept_total'] <=
                row['max_accept'] * row['active']):
            raise ValueError('invalid receive window range')
    elif any(row[k] for k in FIELDS[3:5] + FIELDS[7:21]):
        raise ValueError('nonzero active observation without PCBs')
    if not row['heap_capacity'] or not row['heap_used'] <= row['heap_peak'] <= row['heap_capacity']:
        raise ValueError('invalid native heap sample')
    return row


def summarize(output):
    rows = {}
    prefix = 'RA4_TLS_RESOURCE:TCP_RX_BUDGET='
    last_key = (0, -1)
    for line in output.replace('\r', '').splitlines():
        if not line.startswith(prefix):
            continue
        round_text, label, values = line[len(prefix):].split(',', 2)
        if not round_text.isdecimal() or int(round_text) < 1 or label not in LABELS:
            raise ValueError('invalid TCP RX budget boundary')
        key = (int(round_text), LABELS.index(label))
        if key in rows or key <= last_key:
            raise ValueError('duplicate or out-of-order TCP RX budget boundary')
        rows[key] = dict(round=key[0], label=label, **decode(values))
        last_key = key
    rounds = sorted({key[0] for key in rows})
    if not rounds or rounds != list(range(1, len(rounds) + 1)):
        raise ValueError('missing TCP RX budget rounds')
    result = []
    for r in rounds:
        for index in range(len(LABELS)):
            if (r, index) not in rows:
                raise ValueError('missing TCP RX budget boundary')
            result.append(rows[r, index])
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(args.log.read_text()), indent=2))
