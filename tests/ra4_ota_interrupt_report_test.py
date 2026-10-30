"""Never accept completed or failed programming as an active interruption."""
import contextlib
import io
from pathlib import Path
import sys
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
import run_ra4_ota_update_final_carrier as u

def report(active,succeeded,verified,error=0):
    b=bytearray(64);b[:6]=b"U35D"+bytes((u.FEATURE_VERSION,u.PROGRAMMER_PAGE))
    b[6]=4;b[7]=error;b[8]=active;b[9]=succeeded
    for offset,value,size in [(10,1,4),(30,52104,4),(34,verified,4),(38,13,2)]:
        b[offset:offset+size]=value.to_bytes(size,"little")
    return bytes(b)

for value,threshold,expected in [
    (report(1,0,4096),4096,False),
    (report(0,1,52104),0,True),
    (report(0,1,52104),4096,"fail"),
    (report(0,0,4096,3),4096,"fail"),
]:
    with patch.object(u,"read_programmer_report",return_value=value),contextlib.redirect_stdout(io.StringIO()):
        try: result=u.wait_for_programmer(1,0,52104,.1,threshold)
        except RuntimeError: assert expected=="fail"
        else: assert result is expected
print("OTA_ACTIVE_INTERRUPTION_REPORT_GATE_PASS")
