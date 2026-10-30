"""Reject unavailable/malformed counter pages and permit zero failures."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from inspect_bl616cl_runtime_allocations import parse, FEATURE_VERSION

for page in (40, 41):
    report = bytearray(64)
    report[:8] = b'U35D' + bytes((FEATURE_VERSION, page, 1, 3))
    assert parse(report, page)[0]['calls'] == 0
    report[8:12] = (10).to_bytes(4, 'little')
    report[12:16] = (2).to_bytes(4, 'little')
    report[16:20] = (8192).to_bytes(4, 'little')
    report[20:24] = (4096).to_bytes(4, 'little')
    assert parse(report, page)[0]['max_failed_request'] == 4096
    for offset, value in ((5, 39), (6, 0), (7, 2), (12, 11), (23, 1)):
        invalid = report.copy()
        invalid[offset] = value
        try:
            parse(invalid, page)
        except RuntimeError:
            pass
        else:
            raise AssertionError((page, offset, value))
    try:
        parse(report[:63], page)
    except RuntimeError:
        pass
    else:
        raise AssertionError('short report accepted')
print('BL616CL_RUNTIME_ALLOCATION_REPORT_PASS')
