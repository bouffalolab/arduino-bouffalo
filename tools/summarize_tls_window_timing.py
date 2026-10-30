#!/usr/bin/env python3
"""Decode cumulative AT-task phase counters around each TLS receive burst."""
import argparse
import json
from pathlib import Path


def decode(values):
    fields = values.split(',')
    if len(fields) != 27 or any(not field.isdecimal() for field in fields):
        raise ValueError('invalid TLS timing width or numeric field')
    numbers = list(map(int, fields))
    if numbers[:3] != [1, 1, 0] or any(n > 0xffffffff for n in numbers):
        raise ValueError('disabled, overflowed or unknown TLS timing layout')
    groups = [numbers[i:i+8] for i in (3, 11, 19)]
    for group in groups:
        count, paused = group[:2]
        if paused > count or any(peak > total for peak, total in zip(group[5:], group[2:5])):
            raise ValueError('inconsistent TLS phase counts or maxima')
        if count == 0 and any(group):
            raise ValueError('nonzero timing without samples')
    return groups


def summarize(output):
    rows = {}
    prefix = 'RA4_TLS_RESOURCE:TLS_WINDOW_TIMING='
    for line in output.replace('\r', '').splitlines():
        if not line.startswith(prefix): continue
        round_text, label, values = line[len(prefix):].split(',', 2)
        if not round_text.isdecimal() or int(round_text) < 1 or label not in ('BEFORE_RX', 'AFTER_RX'):
            raise ValueError('failed or invalid TLS receive timing interval')
        key = int(round_text), label
        if key in rows: raise ValueError('duplicate TLS timing interval')
        rows[key] = decode(values)
    rounds = sorted({key[0] for key in rows})
    if not rounds or rounds != list(range(1, len(rounds)+1)):
        raise ValueError('missing TLS timing rounds')
    result = []
    for round_number in rounds:
        try:
            before, after = (rows[round_number, label] for label in ('BEFORE_RX', 'AFTER_RX'))
        except KeyError as error:
            raise ValueError('missing TLS timing boundary') from error
        for operation, a, b in zip(('available', 'peek', 'receive'), before, after):
            if any(y < x for x, y in zip(a, b)):
                raise ValueError('TLS timing counters decreased')
            count, paused = b[0]-a[0], b[1]-a[1]
            if paused > count: raise ValueError('invalid paused sample delta')
            if operation != 'peek' and count == 0:
                raise ValueError('no TLS receive or availability samples')
            item = dict(round=round_number, operation=operation, count=count, paused=paused)
            for index, phase in enumerate(('pause', 'body', 'resume'), 2):
                total = b[index]-a[index]
                if count == 0 and total: raise ValueError('timing delta without samples')
                item[phase+'_total_us'] = total
                item[phase+'_mean_us'] = total/count if count else None
            # A lifetime max cannot be subtracted to get an interval maximum.
            item['lifetime_max_us'] = dict(zip(('pause', 'body', 'resume'), b[5:]))
            result.append(item)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(args.log.read_text()), indent=2))
