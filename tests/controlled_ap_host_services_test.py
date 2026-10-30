#!/usr/bin/env python3
"""Exercise the actual independent host peer with real UDP and verified TLS."""
import ipaddress
import json
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import threading
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from controlled_ap_host_services import PEER
from run_ra4_wifis3_tls_gate import create_material


class HostPeerTest(unittest.TestCase):
    def test_three_large_datagrams_and_verified_tls(self):
        self.run_peer((64,1024,4096,16384,16384,16384))

    def test_32_consecutive_large_datagrams_and_verified_tls(self):
        self.run_peer((64,1024,4096)+(16384,)*32)

    def test_baseline_boundary_and_fragmented_datagrams_with_tls(self):
        self.run_peer((64,1024,1460,4096,4096,4096))

    def test_original_ap_unfragmented_control_protocol(self):
        observed=[]
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as control:
            control.bind(('127.0.0.1',18464));control.settimeout(10)
            def respond():
                data,source=control.recvfrom(20000)
                observed.append(data);control.sendto(data[:32],source)
            thread=threading.Thread(target=respond);thread.start()
            try:self.run_peer((64,1024,1460,4096,4096,4096),True)
            finally:thread.join(12)
            self.assertFalse(thread.is_alive())
            self.assertEqual(observed,[bytes((i*19+11)%251 for i in range(1460))])

    def run_peer(self, sizes, ap_control=False):
        with tempfile.TemporaryDirectory() as directory, socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as udp:
            work=Path(directory)
            ca,_,_=create_material(work,'ec',ipaddress.IPv4Address('127.0.0.1'))
            (work/'peer.py').write_text(PEER)
            udp.bind(('127.0.0.1',18462));udp.settimeout(10)
            (work/'target').write_text(json.dumps({'sta':'127.0.0.1','udp_sizes':sizes,'ap_fragment_control':ap_control,'gateway':'127.0.0.1'}));(work/'go').touch()
            (work/'ap-fragment-go').touch()
            for sequence in range(1,len(sizes)+1):(work/('udp-go-'+str(sequence))).touch()
            process=subprocess.Popen([sys.executable,str(work/'peer.py'),str(work)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            try:
                for sequence,size in enumerate(sizes):
                    data,source=udp.recvfrom(20000)
                    self.assertEqual(data,bytes((i*17+sequence*29)%251 for i in range(size)))
                    udp.sendto(data,source)
                context=ssl.create_default_context(cafile=str(ca))
                context.maximum_version=ssl.TLSVersion.TLSv1_2
                payload=bytes((i*13+5)%251 for i in range(1024))
                with socket.create_connection(('127.0.0.1',18452),timeout=10) as tcp:
                    with context.wrap_socket(tcp,server_hostname='127.0.0.1') as tls:
                        tls.sendall(payload);reply=b''
                        while len(reply)<len(payload):reply+=tls.recv(1024-len(reply))
                        self.assertEqual(reply,payload)
                        (work/'tls-read').touch()
                stdout,stderr=process.communicate(timeout=10)
                self.assertEqual(process.returncode,0,(stdout,stderr))
                events=(work/'events').read_text()
                self.assertNotIn('FAIL',events)
                if ap_control:self.assertIn('HOST_AP_FRAGMENT_CONTROL_DELIVERED',events)
                for marker in ('HOST_UDP_PASS='+str(len(sizes))+',bytes='+str(sizes[-1]),'HOST_TLS_VERIFIED_ECHO_PASS','HOST_SERVICES_COMPLETE'):
                    self.assertIn(marker,events)
            finally:
                if process.poll() is None:process.kill();process.communicate()


if __name__=='__main__':unittest.main()
