import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from inspect_bl616cl_heap_ownership import FEATURE_VERSION, parse_pages


def fixture():
    pages = []
    for page in range(42, 46):
        data = bytearray(64)
        data[:8] = b'U35D' + bytes((FEATURE_VERSION, page, 1, 8))
        data[8:12] = (3).to_bytes(4, 'little')
        data[12:14] = (30).to_bytes(2, 'little')
        data[14] = 9
        for i, offset in enumerate((16, 40)):
            index = (page - 42) * 2 + i
            data[offset:offset + 4] = (0x60fc1000 + index * 0x1000).to_bytes(4, 'little')
            data[offset + 4:offset + 8] = (8192 - index * 512).to_bytes(4, 'little')
            data[offset + 8] = 1
            data[offset + 9:offset + 17] = b'loopTask'
        pages.append(data)
    return pages


rows = parse_pages(fixture())
assert len(rows) == 8 and rows[0]['task'] == 'loopTask' and rows[0]['size'] == 8192
for page_index, offset, value in ((0, 6, 0), (0, 7, 7), (1, 8, 4),
                                 (2, 14, 10), (0, 24, 3), (0, 16, 0), (0, 45, 255)):
    pages = fixture()
    if offset == 16:
        pages[page_index][16:20] = bytes(4)
    else:
        pages[page_index][offset] = value
    try:
        parse_pages(pages)
    except RuntimeError:
        pass
    else:
        raise AssertionError((page_index, offset, value))
print('BL616CL_HEAP_OWNERSHIP_REPORT_PASS')
