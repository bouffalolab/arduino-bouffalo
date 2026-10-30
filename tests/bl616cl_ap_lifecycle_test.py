"""Validate bounded AP telemetry decoding and exclude pointer/detail fields."""
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from inspect_bl616cl_ap_lifecycle import decode
from inspect_bl616cl_gpio35 import FEATURE_VERSION


class LifecycleTest(unittest.TestCase):
    def frame(self,page):
        data=bytearray(64);data[:6]=b'U35D'+bytes((FEATURE_VERSION,page));return data

    def test_start_ignores_pointer_fields(self):
        data=self.frame(2);data[11]=1;data[12]=6
        data[32:56]=b'\xff'*24
        result=decode(data,2)
        self.assertEqual(result['ap_started'],1);self.assertEqual(result['channel'],6)
        self.assertNotIn(0xffffffff,result.values())

    def test_state_records(self):
        data=self.frame(26);data[7]=3
        for n in range(3):
            offset=12+n*12;data[offset:offset+4]=(n+10).to_bytes(4,'little')
            data[offset+4:offset+10]=bytes((7,1,2,3,4,5))
        records=decode(data,26)['records']
        self.assertEqual(len(records),3);self.assertEqual(records[-1]['sequence'],12)
        self.assertEqual(records[-1]['vif_after'],5)
        data[7]=4
        with self.assertRaises(ValueError):decode(data,26)

    def test_lifecycle_bounds_and_private_detail_exclusion(self):
        data=self.frame(24);data[32]=3;data[16:30]=b'\xff'*14
        result=decode(data,24)
        self.assertEqual(len(result['records']),3)
        self.assertNotIn(0xffffffff,result.values())
        data[32]=4
        with self.assertRaises(ValueError):decode(data,24)

    def test_python_windows_and_firmware_version_contract(self):
        import re
        root=Path(__file__).resolve().parents[1]
        firmware=(root/'UNOR4BL616CLBridge/usb_cmsis_dap.cpp').read_text()
        windows=(root/'tools/verify_bl616cl_cmsis_dap_windows.cs').read_text()
        firmware_version=int(re.search(r'kGpio35FeatureVersion = (\d+)U;',firmware)[1])
        windows_version=int(re.search(r'DiagnosticFeatureVersion = (\d+);',windows)[1])
        self.assertEqual(FEATURE_VERSION,firmware_version)
        self.assertEqual(windows_version,firmware_version)

    def test_failed_association_is_sampled_before_cleanup(self):
        import tempfile
        from unittest.mock import MagicMock, patch
        import run_bl616cl_tcp_rx_multi_softap_gate as gate
        events=[]
        with tempfile.TemporaryDirectory() as directory:
            work=Path(directory)/'gate'
            session=MagicMock()
            session.connect_prepared.side_effect=RuntimeError('association failure')
            session.close.side_effect=lambda: events.append('network_cleanup')
            helper=MagicMock();helper.attach.side_effect=lambda: events.append('attach')
            helper.detach.side_effect=lambda: events.append('detach')
            peer=MagicMock();peer.poll.return_value=None
            def start_peer(*args,**kwargs):
                (work/'events').write_text('ISOLATION_PEER_READY\n')
                return peer
            def transact(link,request,timeout):
                if request==b'AT\r\n':return b'OK\r\n'
                if request==b'AT+WIFIMODE=2\r\n':return b'+WIFIMODE: \r\nOK\r\n'
                if request==b'AT+WIFIMODE=1\r\n':return b'+WIFIMODE: \r\nOK\r\n'
                if request.startswith(b'AT+BEGINSOFTAP='):return b'+BEGINSOFTAP: 1\r\nOK\r\n'
                raise AssertionError('unexpected AT operation')
            def ap_snapshot():
                events.append('snapshot');return {'2':{'attempts':0}}
            with patch.object(sys,'argv',['gate','--work-dir',str(work),'--bad-count','2']), \
                 patch.object(gate.subprocess,'run'), \
                 patch.object(gate.subprocess,'Popen',side_effect=start_peer), \
                 patch.object(gate,'ensure_bl_usb_mapping',return_value='/dev/test'), \
                 patch.object(gate.control,'require_final_carrier_bridge'), \
                 patch.object(gate.control,'compile_proxy',return_value=Path('proxy.bin')), \
                 patch.object(gate.control,'upload_proxy'), \
                 patch.object(gate.control,'restore_scan',side_effect=lambda args:events.append('scan_restore')), \
                 patch.object(gate.control,'transact',side_effect=transact), \
                 patch.object(gate,'capture',return_value={}), \
                 patch.object(gate,'capture_ap',side_effect=ap_snapshot), \
                 patch.object(gate,'capture_ra4',return_value={}), \
                 patch.object(gate,'wait_empty'), \
                 patch.object(gate,'wait_ap_ready'), \
                 patch.object(gate,'PreparedUsbSession',return_value=helper), \
                 patch.object(gate,'SoftApTestSession',return_value=session), \
                 patch.object(gate.serial,'Serial'),patch.object(gate.time,'sleep'):
                with self.assertRaisesRegex(RuntimeError,'association failure'):gate.main()
            self.assertEqual(events.count('snapshot'),3)
            last_snapshot=max(i for i,event in enumerate(events) if event=='snapshot')
            self.assertLess(last_snapshot,events.index('network_cleanup'))
            self.assertIn('scan_restore',events)
            self.assertTrue(helper.close.called)

    def test_ra4_diagnostics_exclude_version_reply_bytes(self):
        data=self.frame(34);data[8]=1;data[30:62]=b'x'*32
        result=decode(data,34)
        self.assertEqual(result['active'],1)
        self.assertNotIn('version',result)
        data=self.frame(12);data[20:24]=(123).to_bytes(4,'little')
        self.assertEqual(decode(data,12)['received'],123)
        data=self.frame(9);data[9]=9
        with self.assertRaises(ValueError):decode(data,9)

    def test_ap_acceptance_waits_for_current_native_completion(self):
        from unittest.mock import patch
        import run_bl616cl_tcp_rx_multi_softap_gate as gate
        def state(attempt,stage,enabled):
            return {'2':dict(attempts=attempt,stage=stage,native_result=0,ap_started=int(enabled)),
                    '26':dict(records=[dict(enabled=int(enabled))])}
        with patch.object(gate,'capture_ap',side_effect=(state(7,3,True),state(8,2,False),state(8,3,True))) as read, \
                patch.object(gate.time,'sleep'):
            gate.wait_ap_ready(7)
            self.assertEqual(read.call_count,3)
        with patch.object(gate,'capture_ap',return_value=state(8,3,False)):
            with self.assertRaisesRegex(RuntimeError,'failed'):gate.wait_ap_ready(7)
        with patch.object(gate,'capture_ap',return_value=state(8,3,True)), \
                patch.object(gate.time,'monotonic',side_effect=(0,11)):
            with self.assertRaisesRegex(RuntimeError,'10 seconds'):gate.wait_ap_ready(7)

    def test_wrong_page_and_truncated_frame(self):
        for data,page in ((self.frame(2),25),(self.frame(2)[:-1],2),(self.frame(3),3)):
            with self.assertRaises(ValueError):decode(data,page)


if __name__=='__main__':unittest.main()
