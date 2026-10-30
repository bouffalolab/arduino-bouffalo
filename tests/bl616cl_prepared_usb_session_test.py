"""Persistent helper protocol: failed detach still permits restore and close."""
import subprocess
import sys
import unittest
import contextlib
import io
from unittest.mock import patch
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from bl616cl_prepared_usb_session import PreparedUsbSession, ROOT_CODE
import bl616cl_usb_mapping as mapping


class PreparedSessionTest(unittest.TestCase):
    def test_native_attach_retry_is_bounded_without_repeating_data(self):
        for statuses in ((1,0),(1,1)):
            calls=[]
            routes=[]
            def run(command, **kwargs):
                if command[0]=='ip':
                    routes.append(command)
                    return subprocess.CompletedProcess(command,0,f'default via 172.20.0.{len(routes)} dev eth0\n')
                calls.append((command,kwargs))
                return subprocess.CompletedProcess(command,statuses[len(calls)-1],b'',b'device busy\n')
            output=io.StringIO()
            with patch.object(mapping,'stale_bl_ports',return_value=[]), patch.object(subprocess,'run',side_effect=run), patch.object(sys,'argv',['helper',str(Path(__file__).resolve().parents[1]/'tools'),'3-1']), patch.object(sys,'stdin',io.StringIO('ATTACH\nEXIT\n')), patch('time.sleep'), contextlib.redirect_stdout(output), contextlib.redirect_stderr(io.StringIO()):
                exec(ROOT_CODE,{})
            self.assertEqual(len(calls),2)
            self.assertEqual([c[0][3] for c in calls],['172.20.0.2','172.20.0.3'])
            self.assertTrue(all(c[0][-1]=='3-1' and c[1]['timeout']<=15 for c in calls))
            self.assertIn('BL_USB_SESSION_ATTACH_RETRY code=1',output.getvalue())
            self.assertIn('BL_USB_SESSION_ATTACH_'+('OK' if statuses[-1]==0 else 'FAILED'),output.getvalue())

    def helper(self, fail_detach=False):
        code=r'''
import sys
print('BL_USB_SESSION_READY',flush=True)
for line in sys.stdin:
 command=line.strip()
 if command=='EXIT':break
 result='FAILED' if command=='DETACH' and sys.argv[1]=='1' else 'OK'
 print('BL_USB_SESSION_'+command+'_'+result,flush=True)
'''
        helper=PreparedUsbSession.__new__(PreparedUsbSession)
        helper.process=subprocess.Popen([sys.executable,'-u','-c',code,str(int(fail_detach))],
            stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        helper._expect(b'BL_USB_SESSION_READY',2)
        self.addCleanup(helper.close)
        return helper

    def test_two_cycles_and_idempotent_close(self):
        helper=self.helper()
        for _ in range(2):helper.detach();helper.attach()
        helper.close();helper.close()
        self.assertEqual(helper.process.returncode,0)

    def test_failed_detach_retains_restore_channel(self):
        helper=self.helper(True)
        with self.assertRaises(RuntimeError):helper.detach()
        helper.attach();helper.close()
        self.assertEqual(helper.process.returncode,0)


if __name__=='__main__':unittest.main()
