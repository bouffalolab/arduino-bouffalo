"""Gate result integrity and prepared peer cancellation, without hardware."""
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import bl616cl_ipv6_tcp_windows_peer as peer


class PeerTest(unittest.TestCase):
    def validate(self, sizes, status=0, cycles=1):
        output = ''.join(f'IPV6_TCP_ECHO_PASS bytes_each_direction={size} elapsed_ms=1\n' for size in sizes)
        with contextlib.redirect_stdout(io.StringIO()):
            peer.validate_result(status, output.encode(), b'', cycles)

    def test_failed_detach_still_restores_network_and_scan(self):
        from run_bl616cl_ipv6_tcp_softap_gate import cleanup_actions
        failed=Mock(side_effect=RuntimeError('detach failed'))
        network=Mock(); mapping=Mock(); scan=Mock()
        with contextlib.redirect_stdout(io.StringIO()):
            error=cleanup_actions([('detach',failed),('network',network),('mapping',mapping),('scan',scan)])
        self.assertIsInstance(error, RuntimeError)
        for action in (failed,network,mapping,scan):action.assert_called_once_with()

    def test_exact_three_sizes(self):
        self.validate([64, 4096, 65536])

    def test_cycles_require_every_connection_and_payload(self):
        self.validate([64, 4096, 65536] * 10, cycles=10)
        for sizes in ([64, 4096, 65536] * 9,
                      [64, 4096, 65536] * 10 + [64],
                      [64, 65536, 4096] * 10):
            with self.assertRaises(RuntimeError):
                self.validate(sizes, cycles=10)

    def test_invalid_cycles_rejected_before_process_start(self):
        with patch.object(peer.subprocess, 'Popen') as process:
            for value in (0, -1, 21, True, '10'):
                with self.assertRaises(ValueError):
                    peer.PreparedIPv6Peer(value)
            process.assert_not_called()

    def test_duplicate_missing_wrong_order_and_exit_fail(self):
        for sizes, status in (([64,64,64],0), ([64,4096],0), ([4096,64,65536],0), ([64,4096,65536],1)):
            with self.subTest(sizes=sizes,status=status), self.assertRaises(RuntimeError):
                self.validate(sizes,status)

    def test_invalid_address_never_sent(self):
        obj = peer.PreparedIPv6Peer.__new__(peer.PreparedIPv6Peer)
        obj.process = Mock()
        for address in ('2001:db8::1', 'fe80::1%17', 'not-an-address'):
            with self.assertRaises(ValueError):
                obj.exchange(address)
        obj.process.communicate.assert_not_called()

    def test_cancel_ready_process(self):
        obj = peer.PreparedIPv6Peer.__new__(peer.PreparedIPv6Peer)
        obj.process = Mock()
        obj.process.poll.return_value = None
        obj.close()
        obj.process.communicate.assert_called_once_with(input=b'CANCEL\n',timeout=5)

    def test_not_ready_is_closed(self):
        process = Mock()
        process.stdout.readline.return_value = b'wrong marker\n'
        process.poll.return_value = None
        with patch.object(peer.subprocess,'Popen',return_value=process), patch('select.select',return_value=([process.stdout],[],[])):
            with self.assertRaisesRegex(RuntimeError,'preparation failed'):
                peer.PreparedIPv6Peer()
        process.communicate.assert_called_once_with(input=b'CANCEL\n',timeout=5)

    def test_exited_preparation_keeps_stderr_before_any_traffic(self):
        process = Mock()
        process.stdout.readline.return_value = b''
        process.poll.return_value = 1
        process.communicate.return_value = (b'', b'compiler CS2012 startup failure')
        with tempfile.TemporaryDirectory() as directory:
            failure = Path(directory) / 'startup.log'
            with patch.object(peer.subprocess,'Popen',return_value=process), patch('select.select',return_value=([process.stdout],[],[])), contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(RuntimeError,'preparation failed'):
                    peer.PreparedIPv6Peer(failure_log=failure)
            self.assertEqual(failure.read_bytes(), b'compiler CS2012 startup failure')
        process.communicate.assert_called_once_with(timeout=5)

    def test_transient_wsl_interop_startup_retries_before_ready(self):
        first = Mock()
        first.stdout.readline.return_value = b''
        first.poll.return_value = 1
        first.communicate.return_value = (b'', b'<3>WSL ERROR UtilAcceptVsock: accept4 failed 110')
        second = Mock()
        second.stdout.readline.return_value = b'IPV6_TCP_PEER_READY\n'
        second.poll.return_value = None
        with patch.object(peer.subprocess, 'Popen', side_effect=[first, second]), \
             patch('select.select', side_effect=[([first.stdout], [], []), ([second.stdout], [], [])]), \
             patch('time.sleep'):
            with contextlib.redirect_stdout(io.StringIO()) as output:
                obj = peer.PreparedIPv6Peer()
            self.assertIs(obj.process, second)
            self.assertIn('IPV6_TCP_PEER_PREPARATION_RETRY=1', output.getvalue())
        first.communicate.assert_called_once_with(timeout=5)


if __name__ == '__main__':
    unittest.main()
