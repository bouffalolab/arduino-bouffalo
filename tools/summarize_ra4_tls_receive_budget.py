#!/usr/bin/env python3
"""Account for measured RA4 receive time; never infer a passing pressure gate."""
import argparse
import json
from pathlib import Path
import re

FIELDS = ('available_calls', 'available_at_calls', 'available_empty_calls',
          'available_ms', 'available_at_ms', 'read_calls', 'refill_calls',
          'read_ms', 'refill_ms', 'fetched_bytes')


def decode(text):
    boundaries, slots = {}, {}
    for line in text.splitlines():
        match = re.fullmatch(r'RA4_TLS_RESOURCE:RX_BURST_(BOUNDARY|SLOT_BUDGET)=([0-9,]+)', line)
        if not match:
            continue
        values = list(map(int, match[2].split(',')))
        if match[1] == 'BOUNDARY':
            if len(values) != 3 or values[0] in boundaries or values[2] > 8:
                raise ValueError('invalid or duplicate receive boundary')
            boundaries[values[0]] = values[1:]
        else:
            if len(values) != 12 or not 2000 <= values[1] <= 2007:
                raise ValueError('invalid slot budget layout')
            key = tuple(values[:2])
            if key in slots:
                raise ValueError('duplicate slot budget')
            row = dict(zip(FIELDS, values[2:]))
            if (row['available_at_calls'] > row['available_calls'] or
                    row['available_empty_calls'] > row['available_calls'] or
                    row['refill_calls'] > row['read_calls'] or
                    row['available_at_ms'] > row['available_ms'] or
                    row['refill_ms'] > row['read_ms']):
                raise ValueError('inconsistent slot budget')
            slots[key] = row
    if not boundaries or set(slots) != {(r, s) for r in boundaries for s in range(2000, 2008)}:
        raise ValueError('missing boundary or slot budget; partial capture is not complete evidence')
    reports = []
    for round_number, (elapsed, completed) in sorted(boundaries.items()):
        totals = {field: sum(slots[(round_number, s)][field] for s in range(2000, 2008))
                  for field in FIELDS}
        measured = totals['available_ms'] + totals['read_ms']
        if measured > elapsed:
            raise ValueError('measured sequential calls exceed receive boundary')
        reports.append(dict(round=round_number, elapsed_ms=elapsed, completed=completed,
                            totals=totals, outside_calls_ms=elapsed-measured,
                            plaintext_wire_lower_bound_ms=round(totals['fetched_bytes']*10000/115200, 2)))
    return reports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    args = parser.parse_args()
    print(json.dumps(decode(args.log.read_text(errors='replace')), indent=2))


if __name__ == '__main__':
    main()
