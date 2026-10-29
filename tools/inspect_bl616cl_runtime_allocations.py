#!/usr/bin/env python3
"""Read experimental boot-cumulative allocation counters from paired BL616CL."""
import os
from inspect_bl616cl_gpio35 import (
    FEATURE_VERSION, find_hidraw, read_feature, read_u32_le, select_feature_page,
)


def parse(report, page):
    if len(report) != 64 or report[:6] != b'U35D' + bytes((FEATURE_VERSION, page)):
        raise RuntimeError('unexpected allocation feature page')
    if report[6:8] != b'\x01\x03':
        raise RuntimeError('allocation diagnostic unavailable')
    rows = []
    for offset in (8, 24, 40):
        row = dict(zip(('calls', 'failures', 'max_request', 'max_failed_request'),
                       (read_u32_le(report, offset + i * 4) for i in range(4))))
        if row['failures'] > row['calls'] or row['max_failed_request'] > row['max_request']:
            raise RuntimeError('inconsistent allocation counters')
        rows.append(row)
    return rows


def main():
    hidraw = find_hidraw()
    fd = os.open(hidraw, os.O_RDWR | os.O_NONBLOCK)
    try:
        print('HIDRAW=' + hidraw)
        for page, names in ((40, ('ANY', 'OCRAM', 'WRAM')),
                            (41, ('OTHER', 'OCRAM_ATTEMPT', 'WRAM_ATTEMPT'))):
            select_feature_page(fd, page)
            rows = parse(read_feature(fd), page)
            for name, row in zip(names, rows):
                print('RUNTIME_ALLOC:' + name + '=' + ','.join(f'{k}:{v}' for k, v in row.items()))
        print('RUNTIME_ALLOCATION_TELEMETRY_PASS')
    finally:
        select_feature_page(fd, 0)
        os.close(fd)


if __name__ == '__main__':
    main()
