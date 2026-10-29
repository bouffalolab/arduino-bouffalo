"""Execute bounded close and real runner finally blocks with stalled tty closes."""
import ast
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from bounded_serial_close import close_serial_bounded, SerialCloseTimeout
import run_ra4_wifis3_tls_throughput_gate as throughput
import run_ra4_wifis3_tls_resource_gate as resource


class CloseTest(unittest.TestCase):
    def test_normal_error_and_invalid_timeout(self):
        link = Mock()
        close_serial_bounded(link, phase='test')
        close_serial_bounded(link, phase='repeat')
        link.close.assert_called_once_with()
        bad = Mock(); bad.close.side_effect = OSError('tty disconnected')
        with self.assertRaises(RuntimeError): close_serial_bounded(bad, phase='error')
        for value in (0, -1, float('inf'), float('nan')):
            with self.assertRaises(ValueError):close_serial_bounded(Mock(), phase='invalid', timeout=value)

    def test_failed_close_seals_old_object_against_destructor_retry(self):
        class Link:
            def __init__(self):self.is_open=True;self.calls=0
            def close(self):
                if self.is_open:
                    self.calls+=1
                    raise OSError('interrupted close')
        link=Link()
        with self.assertRaises(RuntimeError):close_serial_bounded(link,phase='seal')
        self.assertFalse(link.is_open)
        link.close()  # Same entry point an IOBase destructor would use.
        self.assertEqual(link.calls,1)

    def test_pending_job_is_not_closed_twice_and_late_completion(self):
        release = threading.Event(); entered = threading.Event()
        link = Mock()
        def stalled():
            entered.set();release.wait()
        link.close.side_effect = stalled
        try:
            start = time.monotonic()
            with self.assertRaises(SerialCloseTimeout):close_serial_bounded(link, phase='stall', timeout=.03)
            self.assertTrue(entered.is_set());self.assertLess(time.monotonic()-start, 1)
            with self.assertRaises(SerialCloseTimeout):close_serial_bounded(link, phase='again', timeout=.03)
            link.close.assert_called_once_with()
        finally:release.set()
        close_serial_bounded(link, phase='late', timeout=1)
        link.close.assert_called_once_with()

    def test_daemon_python_wait_does_not_hold_process_exit(self):
        code = '''
import threading
from types import SimpleNamespace
from bounded_serial_close import close_serial_bounded,SerialCloseTimeout
link=SimpleNamespace(close=lambda:threading.Event().wait())
try:close_serial_bounded(link,phase='child',timeout=.03)
except SerialCloseTimeout:print('CLEANUP_CONTINUED',flush=True)
'''
        env = dict(__import__('os').environ, PYTHONPATH=str(Path(throughput.__file__).parent))
        result=subprocess.run([sys.executable,'-c',code],env=env,capture_output=True,text=True,timeout=3,check=True)
        self.assertIn('CLEANUP_CONTINUED', result.stdout)


def actual_finally(module):
    tree=ast.parse(Path(module.__file__).read_text())
    main=next(node for node in tree.body if isinstance(node,ast.FunctionDef) and node.name=='main')
    blocks=[node for node in main.body if isinstance(node,ast.Try) and node.finalbody]
    body=blocks[-1].finalbody
    result=ast.Module(body=body,type_ignores=[])
    return compile(ast.fix_missing_locations(result),module.__file__,'exec')


class CleanupTest(unittest.TestCase):
    def test_throughput_finally_restores_after_timeout_and_preserves_primary(self):
        for primary in (None, RuntimeError('original pressure failure')):
            release=threading.Event();actions=[]
            link=SimpleNamespace(is_open=True,close=lambda:release.wait())
            ns=dict(vars(throughput), bridge=link,error=primary,peer=object(),detach_helper=None,peer_dir=Path('/unused'),
                    firewall=None,softap_session=SimpleNamespace(close=lambda:actions.append('network')),
                    ra4_modified=True,args=SimpleNamespace(bridge_port='auto',arduino_cli='test'),work_dir=Path('/unused'))
            ns.update(close_serial_bounded=lambda obj,**kw:close_serial_bounded(obj,timeout=.03,**kw),
                      stop_throughput_peer=lambda *a:actions.append('peer'),remove_peer_firewall=lambda *a:None,
                      run_command=lambda *a:actions.append('scan'),shutil=SimpleNamespace(rmtree=lambda *a,**k:None))
            try:
                with patch('bl616cl_usb_mapping.ensure_bl_usb_mapping',return_value='/dev/test'):
                    exec(actual_finally(throughput),ns)
                self.assertEqual(actions,['peer','network','scan'])
                self.assertIsNone(ns['bridge'])
                if primary:self.assertIs(ns['error'],primary)
                else:self.assertIsInstance(ns['error'],SerialCloseTimeout)
            finally:release.set()

    def test_resource_finally_restores_after_close_error_and_preserves_primary(self):
        for primary in (None, RuntimeError('original pressure failure')):
            actions=[];link=Mock(is_open=True);link.close.side_effect=OSError('disconnect')
            ns=dict(vars(resource),bridge=link,primary_error=primary,detach_helper=None,ra4_image_installed=True,
                    ra4_modified=True,softap_session=SimpleNamespace(close=lambda:actions.append('network')),
                    peer=object(),work_dir=Path('/unused'),firewall_rule=None,mixed_udp_firewall=None,
                    peer_event_log=Path('/unused'),peer_output='',ra4_output='',
                    args=SimpleNamespace(failure_swd_snapshot=False,sta_queue_recovery=False,at_trace=False))
            ns.update(timeline=lambda *a:None,stop_resource_peer=lambda *a:actions.append('peer'),
                      remove_peer_firewall=lambda *a:None,read_peer_event_log=lambda *a:'',
                      restore_scan=lambda *a:actions.append('scan'),save_gate_diagnostics=lambda *a:Path('/unused'),
                      shutil=SimpleNamespace(rmtree=lambda *a,**k:None))
            exec(actual_finally(resource),ns)
            self.assertEqual(actions,['network','peer','scan']);self.assertIsNone(ns['bridge'])
            if primary:self.assertIs(ns['primary_error'],primary)
            else:self.assertIsInstance(ns['primary_error'],RuntimeError)


if __name__=='__main__':unittest.main()
