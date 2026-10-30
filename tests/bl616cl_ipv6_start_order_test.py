"""BLE-first must establish live scanning before requesting the AP."""
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import run_bl616cl_ipv6_ble_usb_softap_gate as gate

class StartOrderTest(unittest.TestCase):
    def test_both_orders_require_exact_parameters_and_reports(self):
        for before_ap,command in ((True,b'PRESCAN\n'),(False,b'SCAN\n')):
            link=Mock()
            with patch.object(gate,'marker',side_effect=['32,32','7']), patch.object(gate.time,'sleep'):
                self.assertEqual(gate.start_ble(link,before_ap),7)
            self.assertEqual([call.args[0] for call in link.write.call_args_list],
                             [command,b'SNAP\n'])

    def test_wrong_scan_parameters_or_no_reports_fail(self):
        for responses in (['16,16'],['32,32','0']):
            with patch.object(gate,'marker',side_effect=responses), patch.object(gate.time,'sleep'):
                with self.assertRaises(RuntimeError):gate.start_ble(Mock(),True)

if __name__=='__main__':unittest.main()
