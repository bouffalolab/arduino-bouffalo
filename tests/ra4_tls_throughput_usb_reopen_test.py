"""Reopen after WLAN preparation, without reset or retrying application data."""
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock,patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import run_ra4_wifis3_tls_throughput_gate as gate
import bl616cl_usb_mapping as mapping
class ReopenTest(unittest.TestCase):
    def test_new_fd_on_current_mapping(self):
        old=Mock();new=Mock()
        with patch.object(mapping,'ensure_bl_usb_mapping',return_value='/dev/ttyACM7'),patch.object(gate.serial,'Serial',return_value=new) as serial:
            self.assertIs(gate.reopen_after_association(old),new)
            old.close.assert_called_once_with()
            serial.assert_called_once_with('/dev/ttyACM7',115200,timeout=0.1,write_timeout=1)
            self.assertIs(new.dtr,False);self.assertIs(new.rts,False);new.write.assert_not_called()
    def test_failed_recovery_does_not_open(self):
        old=Mock()
        with patch.object(mapping,'ensure_bl_usb_mapping',side_effect=RuntimeError('unavailable')),patch.object(gate.serial,'Serial') as serial:
            with self.assertRaises(RuntimeError):gate.reopen_after_association(old)
            old.close.assert_not_called();serial.assert_not_called()
if __name__=='__main__':unittest.main()
