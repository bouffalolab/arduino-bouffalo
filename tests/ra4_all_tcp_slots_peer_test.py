"""Exercise all 16 plaintext capacity peers with real sockets and exact bytes."""
from pathlib import Path
import socket
import sys
import tempfile
import threading
import time
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import run_ra4_wifis3_tls_resource_gate as gate

class AllTcpSlotsPeerTest(unittest.TestCase):
    def test_full_capacity_and_rejected_token(self):
        namespace={'__name__':'test_peer'}
        exec(gate.WINDOWS_RESOURCE_PEER,namespace)
        with tempfile.TemporaryDirectory() as directory, socket.socket() as board, socket.socket() as probe:
            board.bind(('127.0.0.1',0));board.listen(16);board.settimeout(3)
            probe.bind(('127.0.0.1',0));port=probe.getsockname()[1];probe.close()
            log=Path(directory)/'events';stop=threading.Event();ready=threading.Event()
            worker=threading.Thread(target=namespace['all_tcp_slots_server'],args=('127.0.0.1',port,board.getsockname()[1],str(log),stop,ready))
            worker.start();links=[]
            try:
                self.assertTrue(ready.wait(3))
                for _ in range(8): links.append(socket.create_connection(('127.0.0.1',port),timeout=3))
                for _ in range(8): links.append(board.accept()[0])
                for link in links:link.settimeout(3);link.sendall(b'X')
                expected=bytes((0x5a+i*19)&255 for i in range(64))
                for link in links:
                    payload=b''
                    while len(payload)<64:payload+=link.recv(64-len(payload))
                    self.assertEqual(payload,expected)
                deadline=time.monotonic()+3
                while time.monotonic()<deadline:
                    text=log.read_text()
                    if text.count('_CONTENT_SENT=')==16:break
                    time.sleep(.01)
                for kind in ('PLAIN','ACCEPTED'):
                    for index in range(1,9):self.assertIn(f'ALL_TCP_{kind}_CONTENT_SENT={index},64',text)
                # A different command must not be silently accepted as a valid probe.
                bad=socket.create_connection(('127.0.0.1',port),timeout=3);inbound=board.accept()[0]
                links.extend([bad,inbound])
                for link in (bad,inbound):link.settimeout(3);link.sendall(b'Q');self.assertEqual(link.recv(1),b'')
                deadline=time.monotonic()+3
                while time.monotonic()<deadline:
                    text=log.read_text()
                    if text.count('ALL_TCP_REQUEST_REJECTED=')==2:break
                    time.sleep(.01)
                self.assertEqual(text.count('ALL_TCP_REQUEST_REJECTED='),2)
                self.assertEqual(text.count('_CONTENT_SENT='),16)
            finally:
                for link in links:link.close()
                stop.set();worker.join(timeout=5)
                self.assertFalse(worker.is_alive())
    def test_budget_and_launch_argument(self):
        self.assertEqual(gate.resource_gate_budget_ms(20,4096,None,False),500000)
        self.assertEqual(gate.resource_gate_budget_ms(20,4096,None,True),900000)
        self.assertEqual(gate.resource_gate_budget_ms(20,4096,123456,True),123456)
        from unittest.mock import patch
        import ipaddress
        with tempfile.TemporaryDirectory() as directory,patch.object(gate.subprocess,'Popen') as popen,patch.object(gate,'wsl_unc_path',side_effect=lambda p:str(p)):
            p=Path(directory)
            gate.launch_windows_peer(p,ipaddress.IPv4Address('127.0.0.1'),1234,p/'cert',p/'key',p/'events',1,False,all_tcp_slots=True)
            command=popen.call_args.args[0][-1]
            self.assertIn('--all-tcp-slots',command)
            self.assertIn('--mixed-tcp-port',command)
if __name__=='__main__':unittest.main()
