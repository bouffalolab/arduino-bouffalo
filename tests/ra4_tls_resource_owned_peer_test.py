from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock,patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import run_ra4_wifis3_tls_resource_gate as gate
class OwnedPeerTest(unittest.TestCase):
    def test_completed_peer_needs_no_windows_query(self):
        peer=Mock();peer.wait.return_value=0
        with patch.object(gate.subprocess,'run') as run:
            gate.stop_resource_peer(peer,Path('/unused'))
            run.assert_not_called()
    def test_only_owned_pid_is_terminated(self):
        peer=Mock();peer.wait.side_effect=[subprocess.TimeoutExpired('peer',1),0]
        with tempfile.TemporaryDirectory() as d,patch.object(gate.subprocess,'run',return_value=subprocess.CompletedProcess([],0)) as run:
            p=Path(d);(p/'windows_tls_resource_peer.pid').write_text('12345')
            gate.stop_resource_peer(peer,p)
            self.assertEqual(run.call_args.args[0],['taskkill.exe','/PID','12345','/T','/F'])
            self.assertEqual(run.call_args.kwargs['timeout'],10)
    def test_invalid_pid_cannot_kill_another_process(self):
        peer=Mock();peer.wait.side_effect=subprocess.TimeoutExpired('peer',1)
        with tempfile.TemporaryDirectory() as d,patch.object(gate.subprocess,'run') as run:
            p=Path(d);(p/'windows_tls_resource_peer.pid').write_text('1 /T')
            with self.assertRaises(RuntimeError):gate.stop_resource_peer(peer,p)
            run.assert_not_called()
if __name__=='__main__':unittest.main()
