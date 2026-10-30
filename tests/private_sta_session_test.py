#!/usr/bin/env python3
"""Verify WLAN selection and restoration on gate and association failures."""
import importlib.util,sys,unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch,call
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('sta_session',ROOT/'tools/hardware_regression/ensure_sta_route.py');s=importlib.util.module_from_spec(spec);spec.loader.exec_module(s)
class Session(unittest.TestCase):
 def invoke(self,current='other-network',status=7,wait_error=False):
  with patch.object(s,'load_private_credentials',return_value=('test-sta','test-password')),patch.object(s,'profile',return_value='original-profile'),patch.object(s,'ssid',return_value=current),patch.object(s,'connect') as connect,patch.object(s,'connect_private') as private,patch.object(s,'wait',side_effect=[RuntimeError('association failed'),None] if wait_error else None) as wait,patch.object(s.subprocess,'run',return_value=SimpleNamespace(returncode=status)) as run,patch.object(sys,'argv',['session','gate-command']):
   if wait_error:
    with self.assertRaisesRegex(RuntimeError,'association failed'):s.main()
    run.assert_not_called()
   else:self.assertEqual(s.main(),status)
   return private.call_args_list+connect.call_args_list,wait.call_args_list
 def test_other_network_is_switched_and_restored_even_when_gate_fails(self):
  connects,waits=self.invoke();self.assertEqual(connects,[call('test-sta','test-password'),call('original-profile')]);self.assertEqual(waits,[call('test-sta'),call('original-profile',by_profile=True)])
 def test_association_failure_still_restores_original_profile(self):
  connects,_=self.invoke(wait_error=True);self.assertEqual(connects,[call('test-sta','test-password'),call('original-profile')])
 def test_existing_correct_sta_does_not_change_user_connection(self):
  connects,waits=self.invoke(current='test-sta',status=0);self.assertEqual(connects,[]);self.assertEqual(waits,[call('test-sta')])
if __name__=='__main__':unittest.main()
