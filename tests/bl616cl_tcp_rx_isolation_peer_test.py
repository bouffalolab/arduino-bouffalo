"""Exercise the real isolation peer protocol with two independent TCP sockets."""
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from run_bl616cl_tcp_rx_isolation import PEER


class IsolationPeerTest(unittest.TestCase):
    def test_tiny_sender_and_exact_independent_exchanges(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            script = work / 'peer.py'
            script.write_text(PEER)
            peer = subprocess.Popen([sys.executable, str(script), '--bind', '127.0.0.1',
                                     '--work', str(work)])
            try:
                deadline = time.monotonic() + 5
                while not (work / 'events').exists():
                    self.assertIsNone(peer.poll())
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(.01)
                with socket.create_connection(('127.0.0.1', 18471), timeout=5) as bad, \
                        socket.create_connection(('127.0.0.1', 18472), timeout=5) as good:
                    for n in range(8):
                        payload = bytes([65+n])*64
                        good.sendall(payload)
                        received = b''
                        while len(received) < 64:
                            part = good.recv(64-len(received))
                            self.assertTrue(part)
                            received += part
                        self.assertEqual(received, payload)
                    tiny = b''
                    while len(tiny) < 64:
                        part = bad.recv(64-len(tiny))
                        self.assertTrue(part)
                        tiny += part
                    self.assertEqual(tiny, b'a'*64)
                    self.assertIsNone(peer.poll())
                self.assertIn('ISOLATION_PEER_EXCHANGE=8', (work/'events').read_text())
                self.assertNotIn('ISOLATION_PEER_FAIL', (work/'events').read_text())
            finally:
                (work/'stop').touch()
                peer.wait(timeout=8)
            self.assertEqual(peer.returncode, 0)


    def test_multiple_senders_and_healthy_reopen(self):
        from run_bl616cl_tcp_rx_multi_softap_gate import expected_blocks, assert_held
        self.assertEqual([expected_blocks(n) for n in (2,4,7)], [32,64,103])
        for count in (2,4,7):
            with self.subTest(count=count), tempfile.TemporaryDirectory() as directory:
                work=Path(directory); script=work/'peer.py'; script.write_text(PEER)
                peer=subprocess.Popen([sys.executable,str(script),'--bind','127.0.0.1',
                    '--work',str(work),'--bad-count',str(count),'--healthy-sessions','2'])
                bad=[]
                try:
                    deadline=time.monotonic()+5
                    while not (work/'events').exists():
                        self.assertIsNone(peer.poll()); self.assertLess(time.monotonic(),deadline)
                        time.sleep(.01)
                    bad=[socket.create_connection(('127.0.0.1',18471+i),timeout=5) for i in range(count)]
                    for session in range(2):
                        with socket.create_connection(('127.0.0.1',18471+count),timeout=5) as good:
                            for n in range(8):
                                payload=bytes([65+n])*64; good.sendall(payload); received=b''
                                while len(received)<64:
                                    part=good.recv(64-len(received)); self.assertTrue(part); received+=part
                                self.assertEqual(received,payload)
                        if session==0:
                            for connection in bad:
                                received=b''
                                while len(received)<64:
                                    part=connection.recv(64-len(received));self.assertTrue(part);received+=part
                                self.assertEqual(received,b'a'*64)
                            (work/'release').touch()
                    deadline=time.monotonic()+2
                    while 'ISOLATION_PEER_EXCHANGE=16' not in (work/'events').read_text():
                        self.assertLess(time.monotonic(),deadline);time.sleep(.01)
                    self.assertNotIn('ISOLATION_PEER_FAIL',(work/'events').read_text())
                    # Aggregate=cap*N proves every owner full only below global
                    # saturation. Seven owners must never be labelled 7*16.
                    view={'blocks':expected_blocks(count),'max_owner_blocks':16,
                          'blocks_reserved':32-count}
                    assert_held(view,count)
                    with self.assertRaises(RuntimeError):
                        assert_held(dict(view,blocks=view['blocks']-1),count)
                finally:
                    for connection in bad:connection.close()
                    (work/'stop').touch();peer.wait(timeout=8)
                self.assertEqual(peer.returncode,0)


    def test_empty_observation_after_deadline_is_rejected(self):
        from unittest.mock import patch
        import run_bl616cl_tcp_rx_multi_softap_gate as gate
        empty=dict.fromkeys(('sequence_used','physical_used','blocks','live_flows',
                             'retired_flows','type_rejected'),0)
        with patch.object(gate,'capture',return_value=empty), \
                patch.object(gate.time,'monotonic',side_effect=(0,11)):
            with self.assertRaisesRegex(RuntimeError,'deadline'):
                gate.wait_empty()

    def test_empty_requires_complete_future_reserve(self):
        from unittest.mock import patch
        import run_bl616cl_tcp_rx_multi_softap_gate as gate
        empty=dict.fromkeys(('sequence_used','physical_used','blocks','live_flows',
                             'retired_flows','type_rejected'),0)
        with patch.object(gate,'capture',side_effect=(empty,dict(physical_reserved=8192,blocks_reserved=31))):
            with self.assertRaisesRegex(RuntimeError,'reserve'):
                gate.wait_empty()


if __name__ == '__main__':
    unittest.main()
