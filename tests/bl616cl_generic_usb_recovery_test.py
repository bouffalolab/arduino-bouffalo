"""Verify USB attach and FTDI hold cleanup without stale gateway/COM guesses."""
from pathlib import Path
import contextlib
import io
import subprocess
import sys
import tempfile
from unittest.mock import patch, Mock
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import run_bl616cl_generic_at_final_carrier as gate

for physical, failure in ((False,False), (True,False), (False,True), (True,True)):
    with tempfile.TemporaryDirectory() as temp, contextlib.ExitStack() as stack:
        stack.enter_context(patch.object(gate, 'BUILD', Path(temp)))
        stack.enter_context(patch.object(gate, 'wsl_unc_path', side_effect=str))
        stack.enter_context(patch.object(gate, 'run'))
        stack.enter_context(patch.object(gate, 'require_final_carrier_bridge'))
        stack.enter_context(patch.object(gate.time, 'sleep'))
        stack.enter_context(patch.object(gate, 'resolve_paired_bl616cl_port',
            side_effect=[RuntimeError('not attached'), '/dev/test']))
        # Real mkstemp uses os.open internally, so replace only the device open.
        original_open = gate.os.open
        def open_device(path, *args, **kwargs):
            if path == '/dev/test':
                assert bool(list(Path(temp).glob('*.hold'))) == physical
                return original_open('/dev/null', *args, **kwargs)
            return original_open(path, *args, **kwargs)
        stack.enter_context(patch.object(gate.os, 'open', side_effect=open_device))
        mapping = stack.enter_context(patch.object(gate, 'ensure_bl_usb_mapping', return_value='/dev/test'))
        if failure:
            mapping.side_effect = subprocess.TimeoutExpired('dynamic dedicated attach', 15)
        reset = Mock(returncode=0)
        def release(*args, **kwargs):
            assert not list(Path(temp).glob('*.hold'))
            return ('BL616CL_FTDI_HOLD_RELEASED\n', None)
        reset.communicate.side_effect = release
        def launch_reset(*args, **kwargs):
            kwargs['stdout'].write('BL616CL_FTDI_HOLD_ACTIVE\n')
            kwargs['stdout'].flush()
            return reset
        launch = stack.enter_context(patch.object(gate.subprocess, 'Popen', side_effect=launch_reset))
        stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
        if failure:
            try:
                gate.restore_usb(physical_reset=physical)
                raise AssertionError('mapping failure was hidden')
            except subprocess.TimeoutExpired:
                pass
        else:
            gate.restore_usb(physical_reset=physical)
        mapping.assert_called_once_with()
        assert launch.called == physical
        if physical:
            assert 'reset_bl616cl_ftdi_windows.ps1' in str(launch.call_args)
            reset.communicate.assert_called_once()
print('BL616CL_GENERIC_USB_RECOVERY_PASS')
