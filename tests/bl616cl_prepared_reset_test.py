"""Prearming must be cancellable without issuing a reset; firing must be explicit."""
from pathlib import Path
import sys
import tempfile
from unittest.mock import Mock,patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
import prepared_bl616cl_reset as r

for fire in (False,True):
    with tempfile.TemporaryDirectory() as directory:
        work=Path(directory)
        process=Mock();process.poll.return_value=None;process.wait.return_value=0;process.returncode=0
        def start(command,**kwargs):
            Path(command[command.index("-ReadyPath")+1]).write_text("ready")
            return process
        with patch.object(r,"resolve_paired_bl616cl_port",return_value="/dev/test"),\
             patch.object(r,"require_final_carrier_bridge"),patch.object(r,"wsl_unc_path",side_effect=str),\
             patch.object(r.subprocess,"Popen",side_effect=start),patch.object(r,"restore_usb") as restore:
            reset=r.PreparedBlReset(work)
            assert not reset.trigger.exists()
            if fire:
                reset.log.write_text("BL616CL_FTDI_FLASH_BOOT_RESET_PASS")
                reset.fire()
                restore.assert_called_once_with(physical_reset=False)
                assert reset.trigger.exists()
            else:
                restore.assert_not_called()
            reset.close()
            assert not reset.hold.exists()
print("BL616CL_PREPARED_RESET_CANCEL_AND_EXPLICIT_FIRE_PASS")
