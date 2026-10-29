"""Host permission denial must leave the RA4 Sketch and WLAN untouched."""
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import run_ra4_wifis3_tls_throughput_gate as gate


class FirewallPreflightTest(unittest.TestCase):
    def check_denied(self, softap):
        with tempfile.TemporaryDirectory() as directory, contextlib.ExitStack() as stack:
            def mock(name, **kwargs):
                return stack.enter_context(patch.object(gate, name, **kwargs))

            stack.enter_context(patch.object(sys, "argv", ["gate"] + (["--softap"] if softap else [])))
            stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
            mock("WORK_ROOT", new=Path(directory))
            mock("windows_wlan_address", return_value="192.0.2.1")
            mock("create_material", return_value=(Path("ca"), Path("cert"), Path("key")))
            mock("compile_sketch", return_value=Path("image.bin"))
            mock("resolve_paired_bl616cl_port", return_value="/dev/test-bl")
            stack.enter_context(patch.object(gate.final_scan, "require_final_carrier_bridge"))
            mock("load_private_credentials", return_value=("test-ssid", "test-password"))
            firewall = mock("configure_peer_firewall", side_effect=RuntimeError("permission denied"))
            cleanup = mock("remove_peer_firewall")
            upload = mock("run_command")
            link = stack.enter_context(patch.object(gate.serial, "Serial"))
            session = stack.enter_context(patch("bl616cl_softap_test_session.SoftApTestSession"))
            stack.enter_context(patch("bl616cl_usb_mapping.ensure_bl_usb_mapping", return_value="/dev/test-bl"))
            helper = Mock()
            helper.poll.return_value = 0
            stack.enter_context(patch("run_ra4_wifis3_tls_resource_gate.start_preparation_detach", return_value=helper))

            with self.assertRaisesRegex(RuntimeError, "permission denied"):
                gate.main()

            firewall.assert_called_once_with("192.168.4.100" if softap else "192.0.2.1", gate.TLS_PORT)
            cleanup.assert_called_once_with(None)
            upload.assert_not_called()
            link.assert_not_called()
            session.assert_not_called()

    def test_sta_denied_rule_does_not_upload_or_open_serial(self):
        self.check_denied(False)

    def test_softap_denied_rule_does_not_upload_or_start_wlan(self):
        self.check_denied(True)


if __name__ == "__main__":
    unittest.main()
