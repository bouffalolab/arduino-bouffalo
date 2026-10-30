"""Exercise long gate captures without hiding failure or unbounded output."""

import sys
import serial
from unittest.mock import patch
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from run_ra4_wifis3_tls_resource_gate import capture_until, MarkerCaptureError, require_ble_ap_evidence, require_ra4_boundary_evidence, parse_ap_join_diagnostic, AP_JOIN_FIELDS
from run_ra4_wifis3_tls_resource_gate import restore_scan, upload_sketch


class Link:
    def __init__(self, payload):
        self.payload = payload
        self.writes = []

    def read(self, count):
        result, self.payload = self.payload[:count], self.payload[count:]
        return result

    def write(self, payload):
        self.writes.append(payload)
        return len(payload)

    def flush(self):
        pass


def main():
    args = SimpleNamespace(bridge_port="/dev/ttyACM0", runtime_settle_ms=500)
    with patch("run_ra4_wifis3_tls_resource_gate.ensure_bl_usb_mapping", return_value="/dev/ttyACM2"), \
            patch("run_ra4_wifis3_tls_resource_gate.run_command") as command:
        upload_sketch(args, Path("image.bin"))
        assert args.bridge_port == "/dev/ttyACM2"
        assert "/dev/ttyACM2" in command.call_args.args[0]
    with patch("run_ra4_wifis3_tls_resource_gate.ensure_bl_usb_mapping", side_effect=RuntimeError("wrong device")), \
            patch("run_ra4_wifis3_tls_resource_gate.run_command") as command:
        try:
            upload_sketch(args, Path("image.bin"))
        except RuntimeError:
            pass
        else:
            raise AssertionError("identity failure must stop upload")
        command.assert_not_called()
    args = SimpleNamespace(bridge_port="/dev/ttyACM0", arduino_cli="arduino-cli")
    with patch("run_ra4_wifis3_tls_resource_gate.ensure_bl_usb_mapping", return_value="/dev/ttyACM2"), \
            patch("run_ra4_wifis3_tls_resource_gate.run_command") as command:
        restore_scan(args)
        assert args.bridge_port == "/dev/ttyACM2"
        assert "/dev/ttyACM2" in command.call_args.args[0]
    with patch("run_ra4_wifis3_tls_resource_gate.ensure_bl_usb_mapping", side_effect=RuntimeError("wrong device")), \
            patch("run_ra4_wifis3_tls_resource_gate.run_command") as command:
        try:
            restore_scan(args)
        except RuntimeError:
            pass
        else:
            raise AssertionError("identity failure must stop upload")
        command.assert_not_called()
    prefix = "RA4_TLS_RESOURCE:RA4_BOUNDARY="
    boundary = "".join(prefix + f"1,{stage},536902400,536900000,536902800,536902700,536900100\n"
                       for stage in range(1, 5))
    require_ra4_boundary_evidence(boundary, 1)
    without_mixed = "\n".join(line for line in boundary.splitlines()
                              if not line.startswith(prefix + "1,3,")) + "\n"
    require_ra4_boundary_evidence(without_mixed, 1, mixed_probe=False)
    # Crossings must remain visible evidence rather than being classified as
    # missing diagnostics. These sampled sites cannot prove peak headroom.
    require_ra4_boundary_evidence(boundary.replace("536902800", "536899900")
                                  .replace("536902700", "536899800"), 1)
    for invalid in ("", boundary + boundary, boundary.replace("1,2,", "1,3,"),
                    boundary.replace("536900000", "-1"),
                    boundary.replace("536900000", "0"),
                    boundary.replace("1,4,536902400", "1,4,536902404"),
                    boundary.replace("1,4,536902400,536900000,536902800,536902700,536900100",
                                     "1,4,536902400,536900000,536902800,536902750,536900050"),
                    boundary.replace("1,4,", "1,4,7,")):
        try:
            require_ra4_boundary_evidence(invalid, 1)
        except RuntimeError:
            pass
        else:
            raise AssertionError("invalid boundary evidence accepted")
    # Tie host field names to the actual firmware response arguments, so a
    # shifted field cannot silently be reported as an association completion.
    import re
    root = Path(__file__).resolve().parents[1]
    service = (root / "UNOR4BL616CLBridge/ra4_at_service.cpp").read_text()
    actual = re.findall(r"static_cast<unsigned long>\(join\.(\w+)\.load\(\)\)", service)
    assert actual == list(AP_JOIN_FIELDS)
    record = "RA4_TLS_RESOURCE:AFTER_AP_START_COEX_DIAG="
    values = list(range(36))
    values[5] = 713  # WLAN receive count must never become join elapsed.
    values[29:] = [0, 1, 1, 0, 1, 10000, 0]
    parsed = parse_ap_join_diagnostic(record + ",".join(map(str, values)))
    assert parsed["elapsed_ms"] == 10000 and parsed["completed"] == 0
    assert parsed["client_ip_reply_acknowledged"] == 0
    assert parse_ap_join_diagnostic(record + ",".join(["0"] * 29)) is None
    assert parse_ap_join_diagnostic("missing") is None

    diagnostic = b"RA4_TLS_RESOURCE:DIAG=" + b"0," * 100 + b"\n"
    payload = diagnostic * 500 + b"RA4_TLS_RESOURCE:RESULT=PASS\n"
    result = capture_until(Link(payload), ("RA4_TLS_RESOURCE:RESULT=PASS",),
                           5.0, byte_limit=100 * 4096)
    assert len(result) > 65536 and result.endswith("RESULT=PASS\n")
    link = Link(b"RA4_TLS_RESOURCE:USB_ECHO_READY=1\r\n" + diagnostic * 10 +
                b"RA4_TLS_RESOURCE:USB_ECHO_READY=2\r\n" + diagnostic * 10 +
                b"RA4_TLS_RESOURCE:RESULT=PASS\n")
    capture_until(link, ("RA4_TLS_RESOURCE:RESULT=PASS",), 5.0, usb_echo=True)
    assert link.writes == [b"USB 1\n", b"USB 2\n"]
    # A lost USB/IP mapping must preserve the completed rounds. Never resume
    # or convert a transport interruption into a passing pressure gate.
    class BrokenLink(Link):
        def read(self, count):
            if not self.payload:
                raise serial.SerialException("disconnected")
            return super().read(count)

    partial = b"RA4_TLS_RESOURCE:USB_ECHO_PASS=18\n"
    try:
        capture_until(BrokenLink(partial), ("RA4_TLS_RESOURCE:RESULT=PASS",), 5.0)
    except MarkerCaptureError as error:
        assert "USB_READ_ERROR" in str(error)
        assert error.captured == partial.decode()
    else:
        raise AssertionError("disconnect must remain a failure")

    for stage in ("write", "flush"):
        link = Link(b"RA4_TLS_RESOURCE:USB_ECHO_READY=1\n")
        with patch.object(link, stage, side_effect=serial.SerialException("disconnected")):
            try:
                capture_until(link, ("RA4_TLS_RESOURCE:RESULT=PASS",), 5.0, usb_echo=True)
            except MarkerCaptureError as error:
                assert "USB_WRITE_ERROR" in str(error)
                assert "USB_ECHO_READY=1" in error.captured
            else:
                raise AssertionError("write/flush failure must retain evidence")
    for data, limit, expected in (
        (payload, 65536, "OUTPUT_LIMIT"),
        (diagnostic * 500 + b"RA4_TLS_RESOURCE:RESULT=FAIL:UDP_ACK\n",
         100 * 4096, "FAIL:UDP_ACK"),
    ):
        try:
            capture_until(Link(data), ("RA4_TLS_RESOURCE:RESULT=PASS",),
                          5.0, byte_limit=limit)
        except MarkerCaptureError as error:
            assert expected in str(error)
            assert error.captured.startswith("RA4_TLS_RESOURCE:DIAG=")
        else:
            raise AssertionError("failure must remain visible")
    # Timestamp the first failure diagnostic before RESULT/teardown, once even
    # when its line crosses serial reads. Values must not enter timeline labels.
    evidence = (b"x" * 500 + b"\nRA4_TLS_RESOURCE:EIGHT_FAILURE_SLOT=2003\n" +
                b"RA4_TLS_RESOURCE:ROUND_FAILURE_COEX_DIAG=1,0\n" +
                b"RA4_TLS_RESOURCE:RESULT=FAIL:ROUND\n")
    with patch("run_ra4_wifis3_tls_resource_gate.timeline") as clock:
        try:
            capture_until(Link(evidence), ("RA4_TLS_RESOURCE:RESULT=PASS",), 5.0)
        except MarkerCaptureError:
            pass
        else:
            raise AssertionError("failure expected")
        assert [c.args[0] for c in clock.call_args_list] == [
            "SERIAL_EIGHT_FAILURE_SLOT", "SERIAL_ROUND_FAILURE_COEX_DIAG",
            "RESULT_FAIL_OBSERVED"]
    evidence = "RA4_TLS_RESOURCE:BLE_REPORTS_BEFORE_AP=2\r\nRA4_TLS_RESOURCE:AP_SCAN_RESTORED\nRA4_TLS_RESOURCE:BLE_REPORTS_AFTER_AP=3\n"
    require_ble_ap_evidence(evidence)
    for invalid in ("", evidence.replace("AFTER_AP=3", "AFTER_AP=0"),
                    evidence + evidence, evidence.splitlines()[0] + "\n",
                    evidence.replace("RA4_TLS_RESOURCE:AP_SCAN_RESTORED\n", ""),
                    evidence.replace("RA4_TLS_RESOURCE:AP_SCAN_RESTORED\n", "") + "RA4_TLS_RESOURCE:AP_SCAN_RESTORED\n"):
        try:
            require_ble_ap_evidence(invalid)
        except RuntimeError:
            pass
        else:
            raise AssertionError("missing/zero/duplicate BLE evidence accepted")
    print("RA4_WIFIS3_TLS_RESOURCE_CAPTURE_PASS")


if __name__ == "__main__":
    main()
