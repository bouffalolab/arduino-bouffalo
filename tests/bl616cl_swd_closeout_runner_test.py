"""A recovery gate must repair the interrupted image itself and fail on cleanup."""
import argparse
import contextlib
import io
from pathlib import Path
import sys
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
import run_bl616cl_swd_d8_repeat_program as d8

args=argparse.Namespace(bridge_port="auto",cycles=5,adapter_khz=100,
                       interrupt_after_commands=5,long_session=True)
for failure in (None, 1, 7, 8):
    calls=[]
    def run(command,check=False):
        calls.append(command)
        return argparse.Namespace(returncode=int(len(calls)==failure))
    output=io.StringIO()
    with patch.object(d8,"parse_arguments",return_value=args),patch.object(d8.subprocess,"run",side_effect=run),contextlib.redirect_stdout(output):
        try:
            result=d8.main()
        except RuntimeError:
            assert failure is not None
        else:
            assert failure is None and result==0
    assert "run_ra4_wifis3_scan_final_carrier.py" in calls[-1][1]
    assert all("--skip-restore-scan" in c for c in calls[:-1])
    if failure is None:
        assert len(calls)==8 and "--stop-after-commands" in calls[5]
        assert "--stop-after-commands" not in calls[6]
        assert "LONG_SESSION_PASS" in output.getvalue()
    else:
        assert "LONG_SESSION_PASS" not in output.getvalue()
print("SWD_D8_UNASSISTED_RECOVERY_AND_FAILURE_PROPAGATION_PASS")
