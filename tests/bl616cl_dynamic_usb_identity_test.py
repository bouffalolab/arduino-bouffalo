"""Resolve replugged BL by exact serial, never by tty or physical port order."""
import json
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import bl616cl_usb_mapping as mapping
class DynamicIdentityTest(unittest.TestCase):
    def test_readonly_state_timeout_is_bounded_and_rechecked(self):
        state=json.dumps(dict(Devices=[dict(BusId='3-1',InstanceId='USB\\VID_2341&PID_1002\\BL616CL-PROVISIONAL')])).encode()
        timeout=subprocess.TimeoutExpired(['usbipd.exe','state'],10)
        with patch.object(mapping.subprocess,'run',side_effect=[timeout,subprocess.CompletedProcess([],0,state)]) as run:
            self.assertEqual(mapping.dedicated_bus_id(),'3-1')
            self.assertEqual(run.call_count,2)
        with patch.object(mapping.subprocess,'run',side_effect=timeout) as run:
            with self.assertRaises(subprocess.TimeoutExpired):mapping.dedicated_bus_id()
            self.assertEqual(run.call_count,2)
    def test_replug_and_other_board(self):
        devices=[dict(BusId='3-1',InstanceId='USB\\VID_2341&PID_1002\\BL616CL-PROVISIONAL'),
                 dict(BusId='3-3',InstanceId='USB\\VID_2341&PID_1002\\TESTESP000001')]
        for rows,expected in ((devices,'3-1'),(devices[::-1],'3-1'),
                              ([dict(devices[0],BusId='6-2.4')],'6-2.4')):
            with patch.object(mapping.subprocess,'run',return_value=subprocess.CompletedProcess([],0,json.dumps(dict(Devices=rows)).encode())):
                self.assertEqual(mapping.dedicated_bus_id(),expected)
    def test_missing_duplicate_bootrom_and_invalid_bus_rejected(self):
        target=dict(BusId='3-1',InstanceId='USB\\VID_2341&PID_1002\\BL616CL-PROVISIONAL')
        for rows in ([],[target,target],[dict(target,BusId=None)],
                     [dict(target,BusId='3-1;echo bad')],
                     [dict(target,InstanceId='USB\\VID_FFFF&PID_FFFF\\UNKNOWN')]):
            with patch.object(mapping.subprocess,'run',return_value=subprocess.CompletedProcess([],0,json.dumps(dict(Devices=rows)).encode())):
                with self.assertRaises(RuntimeError):mapping.dedicated_bus_id()
if __name__=='__main__':unittest.main()
