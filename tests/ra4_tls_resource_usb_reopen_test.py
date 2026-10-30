"""Reopen after WLAN preparation, without reset or retrying application data."""
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock,patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import run_ra4_wifis3_tls_resource_gate as gate
from types import SimpleNamespace
class ReopenTest(unittest.TestCase):
    def test_new_fd_on_current_mapping(self):
        old=Mock();new=Mock()
        with patch.object(gate,'ensure_bl_usb_mapping',return_value='/dev/ttyACM7'),patch.object(gate.serial,'Serial',return_value=new) as serial:
            self.assertIs(gate.reopen_after_association(SimpleNamespace(bridge_port="auto"),old),new)
            old.close.assert_called_once_with()
            serial.assert_called_once_with('/dev/ttyACM7',115200,timeout=0.1,write_timeout=1)
            self.assertIs(new.dtr,False);self.assertIs(new.rts,False);new.write.assert_not_called()
    def test_failed_recovery_does_not_open(self):
        old=Mock()
        with patch.object(gate,'ensure_bl_usb_mapping',side_effect=RuntimeError('unavailable')),patch.object(gate.serial,'Serial') as serial:
            with self.assertRaises(RuntimeError):gate.reopen_after_association(SimpleNamespace(bridge_port="auto"),old)
            old.close.assert_not_called();serial.assert_not_called()
    def test_suspend_order_and_identity(self):
        order=Mock();old=order.link;helper=order.helper
        helper.communicate.return_value=(b'BL616CL_PREPARATION_ROOT_DETACH_EXIT\n',b'')
        helper.returncode=0
        args=SimpleNamespace(bridge_port='/dev/ttyACM0')
        with patch.object(gate,'resolve_paired_bl616cl_port',return_value='/dev/ttyACM0'),patch.object(gate.final_scan,'require_final_carrier_bridge') as identity:
            order.attach_mock(identity,'identity')
            gate.suspend_before_association(args,old,helper)
            self.assertEqual([call[0] for call in order.mock_calls],['identity','link.close','helper.communicate'])
            helper.communicate.assert_called_once_with(input=b'DETACH\n',timeout=10)
        with patch.object(gate,'resolve_paired_bl616cl_port',side_effect=RuntimeError('wrong board')):
            with self.assertRaises(RuntimeError):gate.suspend_before_association(args,Mock(),Mock())
        with patch.object(gate,'resolve_paired_bl616cl_port',return_value='/dev/ttyACM0'),patch.object(gate.final_scan,'require_final_carrier_bridge'):
            helper.returncode=3
            with self.assertRaises(RuntimeError):gate.suspend_before_association(args,Mock(),helper)

    def test_root_worker_target_recheck(self):
        import ast,inspect,tempfile,subprocess
        tree=ast.parse(inspect.getsource(gate.start_preparation_detach))
        code=next(node.value.value for node in ast.walk(tree)
                  if isinstance(node,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='code' for t in node.targets))
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);controller=root/'devices/platform/vhci_hcd.0';controller.mkdir(parents=True)
            (controller/'status').write_text('header\nhs 0000 006 003 00040001 000003 1-1\n')
            detach=controller/'detach';detach.write_text('untouched')
            device=root/'bus/usb/devices/1-1';device.mkdir(parents=True)
            for name,value in [('idVendor','2341'),('idProduct','1002'),('serial','BL616CL-PROVISIONAL')]:
                (device/name).write_text(value)
            code=code.replace("'/sys/bus/usb/devices'",repr(str(root/'bus/usb/devices')))
            def run(message):
                return subprocess.run([sys.executable,'-c',code,str(detach),'0'],input=message,
                                      capture_output=True,text=True,timeout=3)
            self.assertEqual(run('CANCEL\n').returncode,2)
            self.assertEqual(detach.read_text(),'untouched')
            (device/'serial').write_text('TESTESP000001')
            self.assertEqual(run('DETACH\n').returncode,4)
            self.assertEqual(detach.read_text(),'untouched')
            (device/'serial').write_text('BL616CL-PROVISIONAL')
            result=run('DETACH\n')
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn('BL616CL_PREPARATION_ROOT_DETACH_EXIT',result.stdout)
            self.assertEqual(detach.read_text(),'0')

    def test_preparation_query_proof(self):
        nonce='1234567890abcdef'
        proof=(f'RA4_TLS_RESOURCE:PREP_EVIDENCE_BEGIN={nonce}\n'
               'RA4_TLS_RESOURCE:PREP_EVIDENCE_ORIGIN=CACHED_PREPARATION\n'
               'RA4_TLS_RESOURCE:PREP_STATE=80,8,30,29,21503,2,0,1\n'
               'RA4_TLS_RESOURCE:BLE_REPORTS_BEFORE_AP=30\n'
               'RA4_TLS_RESOURCE:AP_SCAN_RESTORED\n'
               'RA4_TLS_RESOURCE:BLE_REPORTS_AFTER_AP=29\n'
               f'RA4_TLS_RESOURCE:PREP_EVIDENCE_END={nonce}\n')
        gate.require_preparation_evidence(proof,nonce,True)
        for bad in (proof+proof,proof.replace(nonce,'0'*16),proof.replace('80,8,','70,8,'),
                    proof.replace(',2,0,1',',1,0,1'),proof.replace(',2,0,1',',2,0,2'),
                    proof.replace('AFTER_AP=29','AFTER_AP=0'),proof.replace('AFTER_AP=29','AFTER_AP=28'),
                    proof.replace('RA4_TLS_RESOURCE:AP_SCAN_RESTORED\n',''),
                    proof.replace('RA4_TLS_RESOURCE:BLE_REPORTS_AFTER_AP=29\n',
                                  'RA4_TLS_RESOURCE:BLE_REPORTS_AFTER_AP=29\n'*2)):
            with self.assertRaises(RuntimeError):gate.require_preparation_evidence(bad,nonce,True)

    def test_rejected_query_keeps_evidence_without_resending(self):
        import contextlib,io
        nonce='1234567890abcdef'
        proof=(f'RA4_TLS_RESOURCE:PREP_EVIDENCE_BEGIN={nonce}\n'
               'RA4_TLS_RESOURCE:PREP_EVIDENCE_ORIGIN=CACHED_PREPARATION\n'
               'RA4_TLS_RESOURCE:PREP_STATE=80,8,30,29,21503,2,0,1\n'
               'RA4_TLS_RESOURCE:BLE_REPORTS_BEFORE_AP=30\n'
               'RA4_TLS_RESOURCE:AP_SCAN_RESTORED\n'
               f'RA4_TLS_RESOURCE:PREP_EVIDENCE_END={nonce}\n')
        link=Mock();link.write.side_effect=len
        output=io.StringIO()
        with patch.object(gate.secrets,'token_hex',return_value=nonce), \
                patch.object(gate,'capture_until',return_value=proof), \
                contextlib.redirect_stdout(output):
            with self.assertRaises(RuntimeError):gate.query_preparation_evidence(link,True)
        link.write.assert_called_once_with(f'@PREPSTATUS {nonce}\n'.encode())
        self.assertIn('RA4_TLS_RESOURCE_PREPARATION_QUERY_REJECTED_BEGIN',output.getvalue())
        self.assertIn(proof.strip(),output.getvalue())
if __name__=='__main__':unittest.main()
