"""Exercise OTA peer payload, progress and diagnostic FIN with real TCP."""
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from run_ra4_ota_download_final_carrier import WINDOWS_OTA_PEER


def check_peer(diagnostics):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        script = root / "peer.py"
        package = root / "ra4.ota"
        script.write_text(WINDOWS_OTA_PEER)
        payload = bytes(range(256)) * 80
        package.write_bytes(payload)
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        command = [sys.executable, str(script), "--bind-ip", "127.0.0.1",
                   "--port", str(port), "--package", str(package),
                   "--chunk-bytes", "512", "--chunk-delay-ms", "0"]
        if diagnostics:
            command.append("--tcp-diagnostics")
            command.append("--tcp-nodelay")
        peer = subprocess.Popen(command, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        try:
            assert peer.stdout.readline().strip() == "HTTP_OTA_PEER:READY"
            with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
                client.sendall(b"GET /ra4.ota HTTP/1.1\r\nHost: localhost\r\n\r\n")
                reply = bytearray()
                while data := client.recv(4096):
                    reply.extend(data)
            output = peer.communicate(timeout=3)[0]
            assert peer.returncode == 0, output
            header, body = bytes(reply).split(b"\r\n\r\n", 1)
            assert body == payload
            assert b"Content-Length: 20480" in header
            progress = [int(line.split("=")[1].split(",")[0])
                        for line in output.splitlines() if ":SEND_PROGRESS=" in line]
            assert progress == [8192, 16384, len(payload)], output
            assert "HTTP_OTA_PEER:PAYLOAD_SENT" in output
            assert ("HTTP_OTA_PEER:CLIENT_CLOSED" in output) == diagnostics
            assert ("HTTP_OTA_PEER:TCP_NODELAY=1" in output) == diagnostics
        finally:
            if peer.poll() is None:
                peer.kill()
                peer.communicate()


for diagnostics in (False, True):
    check_peer(diagnostics)
print("RA4_OTA_PEER_TCP_INTEGRITY_PROGRESS_CLOSE_PASS")
