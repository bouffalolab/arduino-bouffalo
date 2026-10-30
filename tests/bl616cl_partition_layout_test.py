"""Catch destructive layout drift and image overflow into either firmware slot."""
import contextlib
import io
from pathlib import Path
import sys
import tempfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
import check_bl616cl_partition_layout as p

with tempfile.TemporaryDirectory() as directory,contextlib.redirect_stdout(io.StringIO()):
    root=Path(directory);layout=root/"layout.toml";image=root/"image.bin"
    layout.write_text(p.LAYOUT.read_text());image.write_bytes(b"x")
    p.validate(layout,image)
    for before,after in [("size0 = 0x1B0000","size0 = 0x200000"),
                         ("address0 = 0x3E9000","address0 = 0x3E8000"),
                         ("size1 = 0x168000","size1 = 0x100000"),
                         ("address1 = 0xF000","address1 = 0xD000")]:
        layout.write_text(p.LAYOUT.read_text().replace(before,after))
        try:p.validate(layout,image)
        except RuntimeError:pass
        else:raise AssertionError("layout drift accepted")
    layout.write_text(p.LAYOUT.read_text())
    with image.open("wb") as f:f.truncate(0x168001)
    try:p.validate(layout,image)
    except RuntimeError:pass
    else:raise AssertionError("secondary slot overflow accepted")
print("BL616CL_PARTITION_LAYOUT_DRIFT_AND_SLOT_OVERFLOW_REJECTED_PASS")
