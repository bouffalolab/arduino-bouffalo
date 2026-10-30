"""Failure telemetry must stay numeric and tolerate unavailable interfaces."""
import contextlib
import io
import subprocess
import sys
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import run_ra4_ota_update_final_carrier as gate

valid = b"+COEXDIAG: " + b",".join([b"0"] * 36) + b"\r\nOK\r\n"
for response in (valid, b"private-network-private-key", valid[:-1]):
    output = io.StringIO()
    with patch.object(gate.ota_download, "transact", return_value=response), \
            patch.object(gate, "find_hidraw", side_effect=OSError("private-error")), \
            patch.object(gate.subprocess, "run", side_effect=OSError("private-error")), \
            contextlib.redirect_stdout(output):
        gate.capture_association_snapshot(None, 2, "FAILED")
    text = output.getvalue()
    assert "private" not in text
    assert ("FAILED_COEX=" in text) == (response == valid)
    assert text.count("HID_UNAVAILABLE=") == 3

output = io.StringIO()
with patch.object(gate.ota_download, "transact", side_effect=RuntimeError("private-error")), \
        patch.object(gate, "find_hidraw", side_effect=OSError("private-error")), \
        patch.object(gate.subprocess, "run", side_effect=subprocess.TimeoutExpired("hid", 8)), \
        contextlib.redirect_stdout(output):
    gate.capture_association_snapshot(None, 2, "FAILED")
assert "private" not in output.getvalue()
assert "FAILED_COEX_UNAVAILABLE" in output.getvalue()
assert output.getvalue().count("HID_UNAVAILABLE=") == 3

reports = []
for page in (27, 29, 52):
    report = bytearray(64)
    report[:6] = b"U35D" + bytes((gate.FEATURE_VERSION, page))
    if page == 29:
        report[22:30] = b"SECRETIP"  # Address fields must never be exported here.
        report[36] = 1
        report[40:44] = (7).to_bytes(4, "little")
        report[44:48] = (8).to_bytes(4, "little")
        report[48:52] = (9).to_bytes(4, "little")
    if page == 52:
        report[6:8] = bytes((1, 1))
        report[8:12] = (3).to_bytes(4, "little")
        report[20:24] = (4).to_bytes(4, "little")
        report[24:28] = (2).to_bytes(4, "little")
    reports.append(bytes(report))
output = io.StringIO()
with patch.object(gate.ota_download, "transact", return_value=valid), \
        patch.object(gate.subprocess, "run", side_effect=OSError()), \
        patch.object(gate, "find_hidraw", return_value="/fake/hid"), \
        patch.object(gate.os, "open", return_value=42), \
        patch.object(gate.os, "close") as close, \
        patch.object(gate, "select_feature_page") as select, \
        patch.object(gate, "read_feature", side_effect=reports), \
        contextlib.redirect_stdout(output):
    gate.capture_association_snapshot(None, 2, "FAILED")
assert "SECRET" not in output.getvalue()
assert "ap_client_input:7,sta_server_input:8,server_output_all_vifs:9" in output.getvalue()
assert "enabled:1,offer_released:3,offer_discarded:0,offer_acknowledged:0,ack_released:4,ack_discarded:2" in output.getvalue()
assert select.call_args.args == (42, 0)
close.assert_called_once_with(42)

# A retained STA-derived AP address must be replaced explicitly, then checked
# before Windows is told to connect. Never echo the old address on mismatch.
with patch.object(gate.ota_download, "transact", return_value=b"+SOFTAPCONFIG: \r\nOK\r\n") as transact:
    gate.configure_softap_network(None, 2)
assert transact.call_args.args[1] == b"AT+SOFTAPCONFIG=192.168.4.1,192.168.4.1,255.255.255.0\r\n"
for response in (b"+IPSOFTAP: 192.168.4.1\r\nOK\r\n", b"+IPSOFTAP: 192.0.2.9\r\nOK\r\n", b"private-error"):
    output = io.StringIO()
    with patch.object(gate.ota_download, "transact", return_value=response), contextlib.redirect_stdout(output):
        try:
            gate.verify_softap_network(None, 2)
        except RuntimeError as error:
            assert response != b"+IPSOFTAP: 192.168.4.1\r\nOK\r\n"
            assert "192.0.2.9" not in str(error) and "private" not in str(error)
        else:
            assert response == b"+IPSOFTAP: 192.168.4.1\r\nOK\r\n"
    assert "192.0.2.9" not in output.getvalue() and "private" not in output.getvalue()
print("RA4_OTA_ASSOCIATION_NUMERIC_PRIVACY_FAILURE_TOLERANCE_PASS")
