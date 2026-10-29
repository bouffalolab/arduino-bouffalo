#!/usr/bin/env python3
"""Capture the eight largest live OCRAM blocks and exact FreeRTOS matches."""
import os
import time
from inspect_bl616cl_gpio35 import (
    FEATURE_VERSION, find_hidraw, read_feature, read_u32_le, select_feature_page,
)


def parse_pages(pages):
    if len(pages) != 4:
        raise RuntimeError('incomplete ownership snapshot')
    metadata = None
    rows = []
    for page, report in zip(range(42, 46), pages):
        if len(report) != 64 or report[:6] != b'U35D' + bytes((FEATURE_VERSION, page)):
            raise RuntimeError('unexpected ownership feature page')
        if report[6:8] != b'\x01\x08' or read_u32_le(report, 8) == 0:
            raise RuntimeError('ownership snapshot unavailable')
        current = report[6:16]
        if metadata is not None and metadata != current:
            raise RuntimeError('ownership pages changed during read')
        metadata = current
        for offset in (16, 40):
            address, size = read_u32_le(report, offset), read_u32_le(report, offset + 4)
            kind = report[offset + 8]
            if kind > 2 or (address == 0) != (size == 0):
                raise RuntimeError('invalid ownership row')
            name = report[offset + 9:offset + 24].split(b'\0', 1)[0].decode('ascii')
            if address:
                rows.append(dict(address=address, size=size, kind=kind, task=name))
    if rows != sorted(rows, key=lambda row: (-row['size'], row['address'])):
        raise RuntimeError('unordered ownership rows')
    return rows


def main():
    fd = os.open(find_hidraw(), os.O_RDWR | os.O_NONBLOCK)
    try:
        select_feature_page(fd, 43)
        previous = read_u32_le(read_feature(fd), 8)
        select_feature_page(fd, 42)
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            report = read_feature(fd)
            if report[5] == 42 and read_u32_le(report, 8) != previous:
                break
            time.sleep(.01)
        else:
            raise RuntimeError('ownership capture timed out')
        pages = [report]
        for page in (43, 44, 45):
            select_feature_page(fd, page)
            pages.append(read_feature(fd))
        rows = parse_pages(pages)
        print(f"OCRAM_OWNERSHIP:SEQUENCE={read_u32_le(report, 8)}:BLOCKS={int.from_bytes(report[12:14], 'little')}:TASKS={report[14]}")
        for row in rows:
            print(f"OCRAM_BLOCK:ADDRESS=0x{row['address']:08x}:SIZE={row['size']}:KIND={row['kind']}:TASK={row['task']}")
        print('OCRAM_OWNERSHIP_TELEMETRY_PASS')
    finally:
        select_feature_page(fd, 0)
        os.close(fd)


if __name__ == '__main__':
    main()
