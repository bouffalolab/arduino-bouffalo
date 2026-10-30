#!/usr/bin/env python3
"""Read opt-in numeric controller connection history from the paired BL616CL."""
import json
import os
import argparse
from inspect_bl616cl_gpio35 import (
    FEATURE_VERSION, find_hidraw, read_feature, read_u32_le, select_feature_page,
)

FIELDS = ('sequence', 'ms', 'event', 'link', 'role', 'interval_half_slots',
          'event_count', 'flags', 'reason', 'controller_clock_half_slots')


def parse(report, page):
    if (len(report) != 64 or report[:6] != b'U35D' + bytes((FEATURE_VERSION, page))
            or report[6:8] != b'\x01\x01'):
        raise ValueError('controller link diagnostic unavailable or wrong page/schema')
    return [read_u32_le(report, 8 + 4*i) for i in range(10)]


def snapshot(read_page):
    first = parse(read_page(63), 63)
    if first[1] != 16:
        raise ValueError('unexpected controller ring capacity')
    rows = []
    for page in range(64, 80):
        words = parse(read_page(page), page)
        if not words[0]:
            if any(words):
                raise ValueError('uninitialized controller row has data')
            continue
        row = dict(zip(FIELDS, words))
        if row['event'] not in (1, 2, 3) or row['role'] not in (0, 1):
            raise ValueError('invalid controller lifecycle field')
        if row['reason'] > 255 or (row['event'] != 3 and row['reason'] != 0):
            raise ValueError('invalid lifecycle reason')
        rows.append(row)
    if first != parse(read_page(63), 63):
        raise ValueError('controller ring changed during read; snapshot rejected')
    rows.sort(key=lambda row: row['sequence'])
    expected = list(range(max(1, first[0] - 15), first[0] + 1))
    if [row['sequence'] for row in rows] != expected:
        raise ValueError('controller ring has missing/duplicate/stale sequence')
    return {'schema': 1, 'total': first[0], 'entries': rows,
            'scope': 'BL controller numeric lifecycle; event1=start,2=first sync,3=cleanup; '
                     'no peer identity or packet data; timestamps use the BL clock'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--advertising-rx', action='store_true',
                        help='Require the additive numeric RX counters in firmware 0.6.93')
    args = parser.parse_args()
    fd = os.open(find_hidraw(), os.O_RDWR | os.O_NONBLOCK)
    try:
        def read_page(page):
            select_feature_page(fd, page)
            return read_feature(fd)
        result = snapshot(read_page)
        if args.advertising_rx:
            words = parse(read_page(80), 80)
            if any(words[4:]) or words[1] > words[0] or words[2] > words[1] or words[3] > words[0]:
                raise ValueError('invalid advertising RX counters')
            result['advertising_rx'] = dict(zip(
                ('descriptors', 'connect_ind_valid_rx', 'connect_ind_parameters_accepted', 'rx_status_errors'),
                words[:4]))
        print(json.dumps(result, indent=2))
    finally:
        select_feature_page(fd, 0)
        os.close(fd)


if __name__ == '__main__':
    main()
