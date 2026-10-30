#!/usr/bin/env python3
"""Preparation survives query startup timeout, without adding pressure retries."""
from pathlib import Path
from unittest.mock import patch
import subprocess,sys,tempfile
from types import SimpleNamespace
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from bl616cl_softap_test_session import SoftApTestSession
with tempfile.TemporaryDirectory() as temp:
 session=SoftApTestSession(Path(temp));session.ssid='public-test-ap';session.ap_address='192.168.4.1'
 class Process:
  def poll(self):return None
 session.process=Process()
 with patch('bl616cl_softap_test_session.subprocess.run',return_value=subprocess.CompletedProcess([],0)) as run:
  session.request_preparation_rejoin()
  assert run.call_args.args[0]==['netsh.exe','wlan','connect','name=public-test-ap','ssid=public-test-ap','interface=WLAN']
 with patch('bl616cl_softap_test_session.subprocess.run',return_value=subprocess.CompletedProcess([],1)):
  try:session.request_preparation_rejoin()
  except RuntimeError:pass
  else:raise AssertionError('failed connect accepted')
 results=[subprocess.TimeoutExpired('query',10),subprocess.CompletedProcess([],0,'192.168.4.100\n','')]
 with patch('bl616cl_softap_test_session.subprocess.run',side_effect=results) as run:
  assert session.current_address()=='192.168.4.100' and run.call_count==2
 # Repeated query timeouts still terminate at the original preparation deadline.
 with patch('bl616cl_softap_test_session.time.monotonic',side_effect=[0,0,0,31]),patch('bl616cl_softap_test_session.subprocess.run',side_effect=subprocess.TimeoutExpired('query',10)):
  try:session.current_address()
  except RuntimeError:pass
  else:raise AssertionError('unbounded preparation')
 # First-start association must never compile/upload a bootstrap Sketch.
 session=SoftApTestSession(Path(temp))
 def launch(*args,**kwargs):
  session.log.write_text('WINDOWS_SOFTAP_UDP_CLIENT_ASSOCIATED\n')
  return Process()
 with patch('bl616cl_softap_test_session.echo.compile_echo',side_effect=AssertionError('bootstrap compile')),patch('bl616cl_softap_test_session.echo.upload_echo',side_effect=AssertionError('bootstrap upload')),patch('bl616cl_softap_test_session.serial.Serial',side_effect=AssertionError('serial bootstrap')),patch('bl616cl_softap_test_session.wsl_unc_path',side_effect=str),patch('bl616cl_softap_test_session.subprocess.Popen',side_effect=launch):
  session.associate(SimpleNamespace(), 'test-generated', 'test-password', '192.168.4.1')
  assert session.hold.exists() and str(session.ap_address)=='192.168.4.1'
 session.output.close()
 session=SoftApTestSession(Path(temp))
 def prepare_launch(*args,**kwargs):
  assert '-ConnectWhenPath' in args[0][-1]
  session.log.write_text('WINDOWS_SOFTAP_UDP_CLIENT_PROFILE_READY\n')
  return Process()
 with patch('bl616cl_softap_test_session.echo.compile_echo',side_effect=AssertionError('bootstrap compile')),patch('bl616cl_softap_test_session.echo.upload_echo',side_effect=AssertionError('bootstrap upload')),patch('bl616cl_softap_test_session.serial.Serial',side_effect=AssertionError('serial bootstrap')),patch('bl616cl_softap_test_session.wsl_unc_path',side_effect=str),patch('bl616cl_softap_test_session.subprocess.Popen',side_effect=prepare_launch):
  session.prepare_association(SimpleNamespace(), 'test-generated', 'test-password', '192.168.4.1')
  assert not session.connect_when.exists()
  session.log.write_text('WINDOWS_SOFTAP_UDP_CLIENT_ASSOCIATED\n')
  session.connect_prepared()
  assert session.connect_when.exists()
 class PreparedOwner(Process):
  def wait(self, timeout):
   assert not session.hold.exists()
   session.log.write_text('WINDOWS_SOFTAP_UDP_CLIENT_PROFILE_REMOVED\n')
   self.returncode=0
   return 0
 session.process=PreparedOwner()
 session.close()
 assert not session.connect_when.exists() and session.output.closed
 # Profile removal alone cannot hide failed original-network restoration.
 session.output=session.log.open('w')
 class FailedOwner(PreparedOwner):
  def wait(self, timeout):
   super().wait(timeout)
   self.returncode=1
   return 1
 session.process=FailedOwner()
 try:session.close()
 except RuntimeError:pass
 else:raise AssertionError('failed WLAN restoration accepted')
 assert session.output.closed

print('BL616CL_SOFTAP_SESSION_PREPARATION_PASS')
