from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import Mock,patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import run_ra4_wifis3_tls_gate as gate
class CleanupTest(unittest.TestCase):
    def test_completed_peer_avoids_cim(self):
        peer=Mock();peer.wait.return_value=0
        with patch.object(gate.subprocess,'run') as run:
            gate.stop_windows_peer(peer,Path('/tmp/peer'))
            run.assert_not_called();peer.kill.assert_not_called()
    def test_live_peer_cleanup_bounded_and_timeout_visible(self):
        peer=Mock();peer.wait.side_effect=[subprocess.TimeoutExpired('peer',1),0]
        with patch.object(gate,'wsl_unc_path',return_value='test-peer'),patch.object(gate.subprocess,'run',side_effect=subprocess.TimeoutExpired('cleanup',15)) as run:
            with self.assertRaisesRegex(RuntimeError,'cleanup timed out'):gate.stop_windows_peer(peer,Path('/tmp/peer'),cleanup_timeout_seconds=15)
            self.assertEqual(run.call_args.kwargs['timeout'],15)
            peer.kill.assert_called_once_with()
            command=run.call_args.args[0][-1]
            self.assertIn('$_.Name -match',command)
            self.assertIn('$_.CommandLine.Contains($needle)',command)
if __name__=='__main__':unittest.main()
