#!/usr/bin/env python3
"""Prove the test peer distinguishes TLS close_notify from bare TCP EOF."""

import ipaddress
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from run_ra4_wifis3_tls_graceful_close_gate import WINDOWS_PEER
from run_ra4_wifis3_tls_gate import create_material, wait_for_peer_events


def recv_exact(connection, size):
    result = bytearray()
    while len(result) < size:
        part = connection.recv(size - len(result))
        if not part:
            raise RuntimeError("unexpected EOF before test payload")
        result.extend(part)
    return bytes(result)


class ClosePeerTest(unittest.TestCase):
    def test_close_modes(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            ca, certificate, key = create_material(work, "ec", ipaddress.IPv4Address("127.0.0.1"))
            context = ssl.create_default_context(cafile=str(ca))
            context.maximum_version = ssl.TLSVersion.TLSv1_2
            cases = (("graceful", False, 1), ("fin", False, 1), ("half-close", False, 1), ("fin", True, 1), ("mixed", False, 3))
            for mode, truncate_recovery, rounds, baseline in [(*case, False) for case in cases] + [("mixed", True, 3, True)]:
                with self.subTest(mode=mode, truncate_recovery=truncate_recovery, rounds=rounds):
                    with socket.socket() as reservation:
                        reservation.bind(("127.0.0.1", 0))
                        port = reservation.getsockname()[1]
                    events = work / (mode + str(truncate_recovery) + ".events")
                    peer = subprocess.Popen([
                        sys.executable, "-c", WINDOWS_PEER,
                        "--bind-ip", "127.0.0.1", "--port", str(port),
                        "--cert", str(certificate), "--key", str(key),
                        "--event-log", str(events), "--close-mode", mode,
                        "--rounds", str(rounds), "--client-close-mode", "baseline" if baseline else "notify",
                    ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                    try:
                        wait_for_peer_events(events, ("TLS_GRACEFUL_CLOSE_PEER:READY",), 5)
                        for cycle in range(1, rounds + 1):
                            expected = bytes((0x70 + index + cycle - 1) & 0xFF for index in range(32))
                            cycle_mode = ("graceful", "fin", "half-close")[(cycle - 1) % 3] if mode == "mixed" else mode
                            with context.wrap_socket(
                                socket.create_connection(("127.0.0.1", port), timeout=5),
                                server_hostname="127.0.0.1", suppress_ragged_eofs=False,
                            ) as first:
                                first.sendall(expected)
                                self.assertEqual(recv_exact(first, 32), expected)
                                if cycle_mode == "graceful":
                                    self.assertEqual(first.recv(1), b"")
                                    first.unwrap().close()
                                else:
                                    self.assertEqual(recv_exact(first, 32), bytes(byte ^ 0xFF for byte in expected))
                                    with self.assertRaises(ssl.SSLEOFError):
                                        first.recv(1)
                                    # Match bridge stop after a fatal TLS EOF.
                                    socket.socket(fileno=first.detach()).close()
                            with context.wrap_socket(
                                socket.create_connection(("127.0.0.1", port), timeout=5),
                                server_hostname="127.0.0.1",
                            ) as second:
                                second.sendall(expected)
                                self.assertEqual(recv_exact(second, 32), expected)
                                if truncate_recovery:
                                    socket.socket(fileno=second.detach()).close()
                                else:
                                    second.unwrap().close()
                        output, _ = peer.communicate(timeout=5)
                        if truncate_recovery and not baseline:
                            self.assertEqual(peer.returncode, 3, output)
                            self.assertIn("TLS_GRACEFUL_CLOSE_PEER:CLIENT_CLOSE_NOTIFY_FAIL", output)
                            self.assertNotIn("TLS_GRACEFUL_CLOSE_PEER:RESULT=PASS", output)
                        else:
                            self.assertEqual(peer.returncode, 0, output)
                            self.assertIn("TLS_GRACEFUL_CLOSE_PEER:CLIENT_TCP_EOF_RECEIVED" if baseline else "TLS_GRACEFUL_CLOSE_PEER:CLIENT_CLOSE_NOTIFY_RECEIVED", output)
                            self.assertEqual(output.count("TLS_GRACEFUL_CLOSE_PEER:CYCLE_PASS="), rounds)
                        if mode == "half-close":
                            self.assertIn("TLS_GRACEFUL_CLOSE_PEER:CLIENT_FIN_RECEIVED", output)
                    finally:
                        if peer.poll() is None:
                            peer.terminate()
                            peer.communicate(timeout=5)


if __name__ == "__main__":
    unittest.main()
