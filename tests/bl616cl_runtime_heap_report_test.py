"""Validate exact heap report decoding and rejected allocator inconsistencies."""
import sys
from pathlib import Path
from contextlib import redirect_stdout
from io import StringIO
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from inspect_bl616cl_gpio35 import parse_runtime_heap, print_runtime_heap, FEATURE_VERSION

def fixture(free):
    data = bytearray(64)
    data[:6] = b'U35D' + bytes((FEATURE_VERSION, 39))
    data[6:10] = (1).to_bytes(4, 'little')
    for offset in (14, 38):
        data[offset] = 63
        data[offset + 2:offset + 4] = (int(free > 0)).to_bytes(2, 'little')
        for delta, value in ((4, free), (8, free), (12, free), (16, 1000), (20, 1000 + free)):
            data[offset + delta:offset + delta + 4] = value.to_bytes(4, 'little')
    return data

for free in (0, 16, 1024):
    data = fixture(free)
    assert parse_runtime_heap(data)[0]['counter_free'] == free
    with redirect_stdout(StringIO()): print_runtime_heap(data)
for offset, value in ((5, 38), (6, 0), (14, 31), (18, 2), (26, 255), (34, 2)):
    data = fixture(16); data[offset] = value
    try:
        with redirect_stdout(StringIO()): print_runtime_heap(data)
    except RuntimeError: pass
    else: raise AssertionError((offset, value))
try: parse_runtime_heap(bytes(63))
except RuntimeError: pass
else: raise AssertionError('short record accepted')
print('BL616CL_RUNTIME_HEAP_REPORT_PASS')
