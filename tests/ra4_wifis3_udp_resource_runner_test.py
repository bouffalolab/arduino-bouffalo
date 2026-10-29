#!/usr/bin/env python3
"""Check UDP gate marker framing without accessing hardware."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from run_ra4_wifis3_udp_resource_gate import capture_udp, PEER


class SerialStub:
    def __init__(self, payload: bytes):
        self.payload = payload

    def read(self, _: int) -> bytes:
        chunk, self.payload = self.payload, b""
        return chunk


lines = ["RA4_UDP_RESOURCE:FOUR_OPEN_PASS",
         "RA4_UDP_RESOURCE:REJECT_PASS",
         *(f"RA4_UDP_RESOURCE:ROUND_PASS={i}" for i in range(1, 4)),
         "RA4_UDP_RESOURCE:REBIND_PASS"]
overlap = ["RA4_UDP_RESOURCE:OVERLAP_RECEIVE_PENDING_PASS",
           "RA4_UDP_RESOURCE:OVERLAP_16K_SEND_PASS",
           "RA4_UDP_RESOURCE:OVERLAP_RECEIVE_READ_PASS",
           "RA4_UDP_RESOURCE:OVERLAP_PEER_ACK_PASS"]
single_cycle_payload = "\r\n".join(
    lines + overlap + ["RA4_UDP_RESOURCE:RESULT=PASS"]
).encode("ascii")
assert "RA4_UDP_RESOURCE:RESULT=PASS" in capture_udp(
    SerialStub(single_cycle_payload), 0.2, 1
)
payload = "\r\n".join(
    lines + ["RA4_UDP_RESOURCE:CYCLE_PASS=1"] +
    lines + ["RA4_UDP_RESOURCE:CYCLE_PASS=2"] +
    lines + overlap + ["RA4_UDP_RESOURCE:CYCLE_PASS=3",
             "RA4_UDP_RESOURCE:ELAPSED_MS=17000",
             "RA4_UDP_RESOURCE:RESULT=PASS"]
).encode("ascii")
assert "RA4_UDP_RESOURCE:RESULT=PASS" in capture_udp(SerialStub(payload), 0.2, 3)
try:
    capture_udp(SerialStub(payload.replace(
        b"RA4_UDP_RESOURCE:CYCLE_PASS=2\r\n", b"")), 0.2, 3)
except RuntimeError as error:
    assert "missing reconnect cycles" in str(error)
else:
    raise AssertionError("missing reconnect marker was accepted")
try:
    capture_udp(SerialStub(single_cycle_payload.replace(
        b"RA4_UDP_RESOURCE:OVERLAP_PEER_ACK_PASS\r\n", b"")), 0.2, 1)
except RuntimeError:
    pass
else:
    raise AssertionError("queue completion without delivery receipt was accepted")

# Exercise the actual peer: only the complete validated 16 KiB datagram
# produces a receipt. No hardware or external network is involved.
import socket
import subprocess
import tempfile
import time

def wait_for_event(path, marker):
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        if marker in path.read_text():
            return
        time.sleep(.01)
    raise AssertionError("peer event was not flushed: " + marker)

with tempfile.TemporaryDirectory() as directory:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    events = Path(directory) / "events"
    peer = subprocess.Popen([sys.executable, "-c", PEER,
        "--bind-ip", "127.0.0.1", "--port", str(port),
        "--event-log", str(events), "--cycles", "1", "--large-datagrams", "3"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 5
        while not events.exists() and time.monotonic() < deadline:
            time.sleep(.01)
        assert events.exists(), "peer did not become ready"
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
            client.settimeout(.2)
            failure_probe = b"bl616cl-udp-post-failure-probe"
            client.sendto(failure_probe, ("127.0.0.1", port))
            client.settimeout(2)
            assert client.recvfrom(128)[0] == failure_probe
            wait_for_event(events, "POST_FAILURE_SMALL_ECHO")
            assert "LARGE_DATAGRAM_RECEIVED=" not in events.read_text()
            client.settimeout(.2)
            client.sendto(bytes(16383), ("127.0.0.1", port))
            try:
                client.recvfrom(128)
            except socket.timeout:
                pass
            else:
                raise AssertionError("short datagram acknowledged")
            client.sendto(bytes(16385), ("127.0.0.1", port))
            try:
                client.recvfrom(128)
            except socket.timeout:
                pass
            else:
                raise AssertionError("truncated oversized datagram acknowledged")
            client.sendto(bytes(16384), ("127.0.0.1", port))
            client.settimeout(2)
            assert client.recvfrom(128)[0] == b"bl616cl-udp-16k-received"
            assert "OVERLAP_16K_RECEIVED" in events.read_text()
            # Wrong content and short packets cannot satisfy a later sequence.
            client.sendto(bytes([1]) * 16383 + bytes([2]), ("127.0.0.1", port))
            client.settimeout(.2)
            try:
                client.recvfrom(128)
            except socket.timeout:
                pass
            else:
                raise AssertionError("corrupt datagram acknowledged")
            for sequence in (1, 2):
                client.sendto(bytes([sequence]) * 16384, ("127.0.0.1", port))
                client.settimeout(2)
                assert client.recvfrom(128)[0] == (
                    b"bl616cl-udp-16k-received-" + str(sequence).encode("ascii"))
                assert f"LARGE_DATAGRAM_RECEIVED={sequence + 1}" in events.read_text()
    finally:
        peer.terminate()
        peer.communicate(timeout=5)

# A final PASS and a receipt for the first datagram cannot cover a missing second.
large_payload = single_cycle_payload.replace(b"RA4_UDP_RESOURCE:RESULT=PASS",
    b"RA4_UDP_RESOURCE:LARGE_DATAGRAM_PASS=1\r\nRA4_UDP_RESOURCE:RESULT=PASS")
try:
    capture_udp(SerialStub(large_payload), .2, 1, 2)
except RuntimeError as error:
    assert "missing large datagram 2" in str(error)
else:
    raise AssertionError("missing second large datagram accepted")

# A successful small diagnostic after a failed large receipt is still a failure.
try:
    capture_udp(SerialStub(b"RA4_UDP_RESOURCE:POST_FAILURE_SMALL_ECHO=PASS\r\n"
                          b"RA4_UDP_RESOURCE:RESULT=FAIL:OVERLAP\r\n"), .2, 1)
except RuntimeError as error:
    assert "gate failed" in str(error)
else:
    raise AssertionError("small liveness probe concealed large datagram loss")


# Exercise varied payload validation in the real peer as well.
with tempfile.TemporaryDirectory() as directory:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    events = Path(directory) / "events"
    peer = subprocess.Popen([sys.executable, "-c", PEER,
        "--bind-ip", "127.0.0.1", "--port", str(port),
        "--event-log", str(events), "--cycles", "1", "--large-datagrams", "3",
        "--varied-payload"], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 5
        while not events.exists() and time.monotonic() < deadline:
            time.sleep(.01)
        assert events.exists()
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
            client.settimeout(.2)
            client.sendto(bytes(16384), ("127.0.0.1", port))
            try:
                client.recvfrom(128)
            except socket.timeout:
                pass
            else:
                raise AssertionError("uniform bytes accepted in varied mode")
            assert "LARGE_PAYLOAD_REJECT=16384,1" in events.read_text()
            for sequence in range(3):
                payload = bytes((sequence + offset * 19) & 255 for offset in range(16384))
                client.sendto(payload, ("127.0.0.1", port))
                client.settimeout(2)
                receipt = b"bl616cl-udp-16k-received"
                if sequence:
                    receipt += b"-" + str(sequence).encode("ascii")
                assert client.recvfrom(128)[0] == receipt
    finally:
        peer.terminate()
        peer.communicate(timeout=5)

print("RA4_WIFIS3_UDP_RESOURCE_RUNNER_PASS")
