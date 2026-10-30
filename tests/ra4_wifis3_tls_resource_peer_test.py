#!/usr/bin/env python3
"""Verify the mixed TLS/UDP gate peer with real local UDP traffic."""

import contextlib
import io
import ipaddress
import ssl
import time
import socket
import sys
import tempfile
import threading
from unittest import mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import run_ra4_wifis3_tls_resource_gate as gate


def check_sta_disconnect_diagnostic() -> None:
    import subprocess
    for outcome, marker in (
        (subprocess.CompletedProcess([], 0, "STA_DISCONNECT=stage=6\n", ""),
         "STA_DISCONNECT=stage=6"),
        (subprocess.CompletedProcess([], 1, "", "fixture error"),
         "STA_DISCONNECT_DIAGNOSTIC_UNAVAILABLE"),
        (subprocess.TimeoutExpired("fixture", 8),
         "STA_DISCONNECT_DIAGNOSTIC_UNAVAILABLE"),
    ):
        output = io.StringIO()
        arguments = {"side_effect": outcome} if isinstance(outcome, Exception) else {"return_value": outcome}
        with mock.patch.object(gate.subprocess, "run", **arguments) as run, contextlib.redirect_stdout(output):
            gate.print_sta_disconnect_diagnostic()
        assert marker in output.getvalue()
        command = run.call_args.args[0]
        assert Path(command[1]).is_file() and command[-1] == "--sta-disconnect"
        assert run.call_args.kwargs["timeout"] == 8


def check_interrupted_restore_failure(cleanup_failure=False) -> None:
    # All hardware, credentials and peer operations are replaced. A planned
    # interruption must still fail if its final scan restoration fails.
    with tempfile.TemporaryDirectory() as directory, contextlib.ExitStack() as stack:
        stack.enter_context(mock.patch.object(gate, "WORK_ROOT", Path(directory)))
        stack.enter_context(mock.patch.object(sys, "argv", [
            "gate", "--interrupt-after-eight-open", "--skip-peer-firewall",
        ]))
        stack.enter_context(mock.patch.object(gate.final_scan, "require_final_carrier_bridge"))
        stack.enter_context(mock.patch.object(gate.serial, "Serial"))
        replacements = {
            "resolve_paired_bl616cl_port": {"return_value": "fixture"},
            "load_private_credentials": {"return_value": ("fixture", "fixture")},
            "windows_wlan_address": {"return_value": "127.0.0.1"},
            "create_material": {"return_value": (Path("ca"), Path("cert"), Path("key"))},
            "compile_sketch": {"return_value": Path("image")},
            "launch_windows_peer": {},
            "wait_for_peer_ready": {"return_value": ""},
            "upload_sketch": {},
            "capture_until": {"side_effect": [
                "RA4_TLS_RESOURCE:READY\n", "RA4_TLS_RESOURCE:INTERRUPT_READY\n",
            ]},
            "wait_for_peer_events": {"return_value": ""},
            "stop_resource_peer": ({"side_effect": RuntimeError("fixture peer cleanup failed")}
                                   if cleanup_failure else {}),
            "remove_peer_firewall": {},
            "read_peer_event_log": {"return_value": ""},
            "restore_scan": {"side_effect": RuntimeError("fixture restore failed")},
            "save_gate_diagnostics": {"return_value": Path("fixture.log")},
        }
        mocked = {}
        for name, arguments in replacements.items():
            mocked[name] = stack.enter_context(mock.patch.object(gate, name, **arguments))
        stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
        stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
        try:
            gate.main()
        except RuntimeError as error:
            expected = "fixture peer cleanup failed" if cleanup_failure else "final-carrier RA4M1 scan restore failed"
            assert expected in str(error)
            mocked["restore_scan"].assert_called_once()
            mocked["save_gate_diagnostics"].assert_called_once()
        else:
            raise AssertionError("interruption hid scan restoration failure")


def check_tls_backpressure_peer(peer) -> None:
    with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
        root = Path(directory)
        _, cert, key = gate.create_material(root, "ec", ipaddress.IPv4Address("127.0.0.1"))
        log = root / "tls.events"
        stop = threading.Event()
        ready = threading.Event()
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        server = threading.Thread(target=peer["tls_server"], args=(
            "127.0.0.1", port, str(cert), str(key), str(log), stop, ready, 2, True,
        ), daemon=True)
        server.start()
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        context.maximum_version = ssl.TLSVersion.TLSv1_2
        context.set_ciphers("ECDHE-ECDSA-AES128-GCM-SHA256")
        try:
            assert ready.wait(2)
            with socket.create_connection(("127.0.0.1", port), timeout=2) as raw:
                with context.wrap_socket(raw, server_hostname="fixture") as secure:
                    secure.sendall(b"bl616cl-queue-hold" + bytes(1000))
                    deadline = time.monotonic() + 2
                    while time.monotonic() < deadline:
                        if "TLS_RESOURCE_PEER:QUEUE_HOLD=1\n" in log.read_text():
                            break
                        time.sleep(0.01)
                    else:
                        raise AssertionError("TLS peer did not enter backpressure mode")
                    secure.settimeout(0.1)
                    try:
                        secure.recv(1)
                    except socket.timeout:
                        pass
                    else:
                        raise AssertionError("held TLS connection unexpectedly returned data or EOF")
        finally:
            stop.set()
            server.join(2)
        assert not server.is_alive(), "TLS listener did not shut down"


def check_receive_burst_peer(peer) -> None:
    with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
        root = Path(directory)
        _, cert, key = gate.create_material(root, "ec", ipaddress.IPv4Address("127.0.0.1"))
        log = root / "burst.events"
        stop, ready = threading.Event(), threading.Event()
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        server = threading.Thread(target=peer["tls_server"], args=(
            "127.0.0.1", port, str(cert), str(key), str(log), stop, ready, 1, False, 4096,
        ), daemon=True)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        context.maximum_version = ssl.TLSVersion.TLSv1_2
        context.set_ciphers("ECDHE-ECDSA-AES128-GCM-SHA256")
        server.start()
        try:
            assert ready.wait(2)
            with contextlib.ExitStack() as stack:
                clients = []
                for _ in range(8):
                    raw = socket.create_connection(("127.0.0.1", port), timeout=2)
                    clients.append(stack.enter_context(context.wrap_socket(raw, server_hostname="fixture")))
                # Include a request split across TLS records; all requests go
                # before the first read, as in the hardware fixture.
                for client in clients:
                    client.sendall(b"bl616cl-rx-")
                    client.sendall(b"burst")
                received = [bytearray() for _ in clients]
                while any(len(data) < 4096 for data in received):
                    for client, data in zip(clients, received):
                        if len(data) < 4096:
                            chunk = client.recv(min(256, 4096-len(data)))
                            assert chunk, "early burst EOF"
                            data.extend(chunk)
                for slot, data in enumerate(received):
                    assert data == bytes((slot+offset*19)&255 for offset in range(4096))
                markers = [f"TLS_RESOURCE_PEER:RX_BURST_SENT={index},4096\n" for index in range(1,9)]
                deadline = time.monotonic() + 2
                while not all(marker in log.read_text() for marker in markers):
                    assert time.monotonic() < deadline, "burst evidence was not flushed"
                    time.sleep(.01)
        finally:
            stop.set()
            server.join(2)
        assert not server.is_alive()


def check_progress_timeout() -> None:
    link = mock.Mock()
    marker = b"RA4_TLS_RESOURCE:STA_QUEUE_OBSERVED=1\n"
    link.read.side_effect = [marker, b""]
    with mock.patch.object(gate.time, "monotonic", side_effect=[0, 0, 0.1, 0.2, 0.3, 0.4]):
        try:
            gate.capture_until(link, ("RA4_TLS_RESOURCE:RESULT=PASS",), 1.0,
                               idle_timeout_seconds=0.1)
        except gate.MarkerCaptureError as error:
            assert "PROGRESS_TIMEOUT" in str(error)
            assert error.captured == marker.decode("ascii")
        else:
            raise AssertionError("stalled STA gate did not retain its last marker")


def check_varied_udp_peer(peer):
    with tempfile.TemporaryDirectory() as directory:
        log = Path(directory) / "events"
        stop, ready = threading.Event(), threading.Event()
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        thread = threading.Thread(target=peer["mixed_udp_server"],
            args=("127.0.0.1", port, 2, str(log), stop, ready, True))
        with contextlib.redirect_stdout(io.StringIO()):
            thread.start()
            try:
                assert ready.wait(2)
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
                    client.settimeout(2)
                    for sequence in (1, 2):
                        header = sequence.to_bytes(4, "little")
                        packet = header + bytes((sequence - 1 + i * 19) & 255
                                                for i in range(4, 16384))
                        client.sendto(packet, ("127.0.0.1", port))
                        assert client.recvfrom(16)[0] == header
                        # Check corruption in the beginning, middle, and end;
                        # a valid header alone cannot earn a delivery receipt.
                        client.settimeout(.2)
                        for offset in (4, 8192, 16383):
                            damaged = bytearray(packet)
                            damaged[offset] ^= 1
                            client.sendto(damaged, ("127.0.0.1", port))
                            try:
                                client.recvfrom(16)
                            except socket.timeout:
                                continue
                            raise AssertionError("corrupt varied UDP packet acknowledged")
                        client.settimeout(2)
                lines = log.read_text().splitlines()
                assert lines.count("TLS_RESOURCE_PEER:MIXED_UDP_REJECT_PAYLOAD") == 6
                for sequence in (1, 2):
                    assert lines.count(f"TLS_RESOURCE_PEER:MIXED_UDP_16K_RECEIVED={sequence}") == 1
            finally:
                stop.set()
                thread.join(2)
                assert not thread.is_alive()


def check_peer_process_startup():
    import subprocess
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        _, cert, key = gate.create_material(root, "ec", ipaddress.IPv4Address("127.0.0.1"))
        script = root / "peer.py"
        # Give the unqueried DNS listener an ephemeral port in this local test.
        script.write_text(gate.WINDOWS_RESOURCE_PEER.replace(
            "link.bind((bind_ip, 53))", "link.bind((bind_ip, 0))"))
        log = root / "events"
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            tls_port = reservation.getsockname()[1]
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
            reservation.bind(("127.0.0.1", 0))
            udp_port = reservation.getsockname()[1]
        process = subprocess.Popen([sys.executable, str(script), "--bind-ip", "127.0.0.1",
            "--port", str(tls_port), "--cert", str(cert), "--key", str(key),
            "--event-log", str(log), "--rounds", "1", "--mixed-udp-port", str(udp_port),
            "--mixed-udp-varied-payload"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                if log.exists() and "TLS_RESOURCE_PEER:READY" in log.read_text():
                    break
                assert process.poll() is None, "complete peer exited before startup"
                time.sleep(.02)
            else:
                raise AssertionError("complete TLS/UDP peer startup failed")
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
                client.settimeout(2)
                packet = (1).to_bytes(4, "little") + bytes(i * 19 & 255 for i in range(4, 16384))
                client.sendto(packet, ("127.0.0.1", udp_port))
                assert client.recvfrom(16)[0] == packet[:4]
        finally:
            process.terminate()
            process.communicate(timeout=5)



def check_parallel_event_log(peer):
    with tempfile.TemporaryDirectory() as directory:
        log = Path(directory) / "events"
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            threads = [threading.Thread(target=lambda slot=slot: [
                peer["emit"](f"TLS_RESOURCE_PEER:FIXTURE={slot},{i}", str(log))
                for i in range(100)]) for slot in range(8)]
            for thread in threads: thread.start()
            for thread in threads: thread.join(5)
            assert not any(thread.is_alive() for thread in threads)
            peer["emit"]("TLS_RESOURCE_PEER:READY", str(log))
        assert output.getvalue() == "TLS_RESOURCE_PEER:READY\n"
        expected = {f"TLS_RESOURCE_PEER:FIXTURE={slot},{i}"
                    for slot in range(8) for i in range(100)}
        lines = log.read_text().splitlines()
        assert len(lines) == 801 and set(lines[:-1]) == expected
        assert lines[-1] == "TLS_RESOURCE_PEER:READY"


def check_tcp_monitor(peer):
    # Exercise the actual embedded Windows peer against the WSAIoctl ABI:
    # success, short report, API error, and an already stopped sampler.
    import ctypes
    for outcome in ("success", "short", "error", "stopped"):
        stop = threading.Event()
        calls = []
        class Ioctl:
            def __call__(self, fd, code, version, version_size, info_ptr,
                         info_size, returned_ptr, overlapped, completion):
                calls.append((fd, code))
                assert code == 0xD8000027 and version._obj.value == 0
                assert version_size == 4 and overlapped is None and completion is None
                info = info_ptr._obj
                info.State, info.Mss = 5, 1460
                info.SndWnd, info.BytesInFlight = 0, 2048
                info.BytesRetrans, info.TimeoutEpisodes = 2920, 2
                returned_ptr._obj.value = info_size - (1 if outcome == "short" else 0)
                # Exactly one sample, no sleeps or thread scheduling assumptions.
                stop.set()
                return -1 if outcome == "error" else 0
        winsock = mock.Mock(WSAIoctl=Ioctl())
        winsock.WSAGetLastError.return_value = 10038
        if outcome == "stopped":
            stop.set()
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "events"
            connection = mock.Mock()
            connection.fileno.return_value = 123
            output = io.StringIO()
            with mock.patch.object(peer["sys"], "platform", "win32"), \
                 mock.patch.object(ctypes, "WinDLL", return_value=winsock, create=True), \
                 contextlib.redirect_stdout(output):
                peer["monitor_tcp"](connection, stop, 7, str(log))
            assert not output.getvalue(), "periodic samples must not fill undrained stdout"
            text = log.read_text() if log.exists() else ""
            if outcome == "success":
                assert "TCP_INFO=7,State:5,Mss:1460" in text
                assert "SndWnd:0" in text and "BytesRetrans:2920" in text
            elif outcome == "stopped":
                assert not calls and not text
            else:
                assert text == "TLS_RESOURCE_PEER:TCP_INFO_UNAVAILABLE=7,10038\n"
            assert not connection.send.called and not connection.recv.called


def main() -> None:
    peer = {"__name__": "resource_peer_test"}
    exec(compile(gate.WINDOWS_RESOURCE_PEER, "resource_peer", "exec"), peer)
    assert gate.resource_gate_budget_ms(20, 0) == 300000
    assert gate.resource_gate_budget_ms(20, 4096) == 500000
    assert gate.resource_gate_budget_ms(3, 4096) == 90000
    assert gate.resource_gate_budget_ms(20, 4096, 300000) == 300000
    check_parallel_event_log(peer)
    check_tcp_monitor(peer)
    check_peer_process_startup()
    check_varied_udp_peer(peer)
    check_progress_timeout()
    check_tls_backpressure_peer(peer)
    check_receive_burst_peer(peer)
    with tempfile.TemporaryDirectory() as directory:
        log = Path(directory) / "peer.events"
        stop = threading.Event()
        ready = threading.Event()
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        thread = threading.Thread(
            target=peer["mixed_udp_server"],
            args=("127.0.0.1", port, 2, str(log), stop, ready),
            daemon=True,
        )
        with contextlib.redirect_stdout(io.StringIO()):
            thread.start()
            try:
                assert ready.wait(2), "peer did not bind"
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
                    client.settimeout(1)
                    # Duplicate valid datagrams get ACKs without inflating evidence.
                    for sequence in (1, 1, 2):
                        header = sequence.to_bytes(4, "little")
                        client.sendto(header + bytes(16380), ("127.0.0.1", port))
                        assert client.recvfrom(16)[0] == header
                    client.settimeout(0.3)
                    invalid_packets = (
                        b"short",
                        bytes(16384),
                        (3).to_bytes(4, "little") + bytes(16380),
                        b"\x01\x00\x00\x00" + bytes(16379) + b"x",
                    )
                    for packet in invalid_packets:
                        client.sendto(packet, ("127.0.0.1", port))
                        try:
                            client.recvfrom(16)
                        except socket.timeout:
                            continue
                        raise AssertionError("invalid datagram received an ACK")
                assert log.read_text().splitlines() == [
                    "TLS_RESOURCE_PEER:MIXED_UDP_16K_RECEIVED=1",
                    "TLS_RESOURCE_PEER:MIXED_UDP_16K_RECEIVED=2",
                    "TLS_RESOURCE_PEER:MIXED_UDP_REJECT_LENGTH=5",
                    "TLS_RESOURCE_PEER:MIXED_UDP_REJECT_SEQUENCE=0",
                    "TLS_RESOURCE_PEER:MIXED_UDP_REJECT_SEQUENCE=3",
                    "TLS_RESOURCE_PEER:MIXED_UDP_REJECT_PAYLOAD",
                ], "peer evidence must count only complete distinct valid datagrams"
            finally:
                stop.set()
                thread.join(2)
                assert not thread.is_alive(), "peer did not stop"
    check_interrupted_restore_failure()
    check_interrupted_restore_failure(cleanup_failure=True)
    check_sta_disconnect_diagnostic()
    print("RA4_WIFIS3_TLS_RESOURCE_PEER_PASS")


if __name__ == "__main__":
    main()
