"""Cover bridge-specific CDC control lines and bounded discovery cleanup."""
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import run_ra4_ble_windows_long_gatt_gate as gate


class RunnerTest(unittest.TestCase):
    def test_hold_requires_order_count_and_each_full_duration(self):
        valid = "WINDOWS_BLE_COEX:HOLD_PASS=1,30000\nWINDOWS_BLE_COEX:HOLD_PASS=2,30001\n"
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gate.validate_hold_evidence(valid, 2, 30), 60001)
        self.assertEqual(gate.validate_hold_evidence("", 2, 0), 0)
        for evidence in ("", valid.replace("=2,", "=3,"), valid.replace("30000", "29999"), valid + valid):
            with self.assertRaises(RuntimeError):
                gate.validate_hold_evidence(evidence, 2, 30)

    def test_controller_feature_control_requires_actual_consistent_observations(self):
        gate.require_controller_cs2([], None)
        gate.require_controller_cs2([0, 0], 0)
        gate.require_controller_cs2([1], 1)
        for values in ([], [-1], [1], [0, 1]):
            with self.assertRaises(RuntimeError):
                gate.require_controller_cs2(values, 0)

    def test_retry_link_loss_does_not_acknowledge_logical_session(self):
        with tempfile.TemporaryDirectory() as temporary:
            signals = Path(temporary)
            self.assertEqual(gate.acknowledge_logical_disconnect(signals, 0), 0)
            self.assertFalse((signals / "disconnect-1.signal").exists())
            (signals / "disconnect-request-1.signal").write_text("REQUEST\n")
            self.assertEqual(gate.acknowledge_logical_disconnect(signals, 0), 1)
            self.assertTrue((signals / "disconnect-1.signal").is_file())
            self.assertEqual(gate.acknowledge_logical_disconnect(signals, 1), 1)
            self.assertFalse((signals / "disconnect-2.signal").exists())

    def test_compatibility_requires_logical_and_physical_evidence(self):
        evidence = ("WINDOWS_BLE_COEX:SESSION_PASS=1\r\nWINDOWS_BLE_COEX:SESSION_PASS=2\r\n"
                    "WINDOWS_BLE_WINRT:DISCOVERY_RETRY=1\r\n")
        with contextlib.redirect_stdout(io.StringIO()) as output:
            gate.validate_backend_compatibility(evidence, 2, 4, 4, 3)
        self.assertIn("LINKS=4:RETRIES=1", output.getvalue())
        for connected, disconnected, logical in ((4, 3, 3), (4, 4, 2), (2, 2, 3)):
            with self.assertRaises(RuntimeError):
                gate.validate_backend_compatibility(evidence, 2, connected, disconnected, logical)
        with self.assertRaises(RuntimeError):
            gate.validate_backend_compatibility(evidence.replace("SESSION_PASS=2", "SESSION_PASS=3"),
                                                2, 4, 4, 3)
        with self.assertRaises(RuntimeError):
            gate.validate_backend_compatibility(evidence.replace("DISCOVERY_RETRY=1", "DISCOVERY_RETRY=2"),
                                                2, 4, 4, 3)

    def test_discovery_failure_probe_trigger(self):
        self.assertEqual(gate.discovery_failure_kind(
            "Could not get GATT services: Unreachable"), "UNREACHABLE")
        self.assertEqual(gate.discovery_failure_kind(
            "WINDOWS_BLE_WINRT:DISCOVERY_CANCELLED=20234,1,1,242\r\nTimeoutError\r\n"),
            "DISCOVERY_TIMEOUT")
        self.assertIsNone(gate.discovery_failure_kind("signal wait\nTimeoutError\n"))
        self.assertIsNone(gate.discovery_failure_kind(
            "WINDOWS_BLE_WINRT:DISCOVERY_CANCELLED=10,1,1,242\nRESULT=PASS\n"))

    def test_original_bridge_uses_dtr_enabled_opener(self):
        with patch.object(gate.esp32_baseline, "open_serial") as original, \
                patch.object(gate.serial, "Serial") as direct:
            link = gate.open_gate_serial("baseline-port", True)
            original.assert_called_once_with("baseline-port", 0.2)
            direct.assert_not_called()
            self.assertIs(link, original.return_value)
            self.assertEqual(link.write_timeout, 2)

    def test_dedicated_bridge_control_lines(self):
        with patch.object(gate.esp32_baseline, "open_serial") as original, \
                patch.object(gate.serial, "Serial") as direct:
            link = gate.open_gate_serial("dedicated-port", False)
            original.assert_not_called()
            direct.assert_called_once_with("dedicated-port", 115200, timeout=0.2, write_timeout=2)
            self.assertFalse(link.dtr)
            self.assertFalse(link.rts)

    def test_cleanup_waits_for_sketch_confirmation(self):
        link = MagicMock()
        link.__enter__.return_value = link
        link.readline.side_effect = [b"READY\r\n", b"RA4_BLE_DISCOVERY:STOPPED\r\n"]
        with patch.object(gate, "open_gate_serial", return_value=link), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(gate.stop_discovery("test-port", False))
        link.write.assert_called_once_with(b"Q")
        link.__exit__.assert_called_once()

    def test_cleanup_open_failure_is_not_confirmed(self):
        with patch.object(gate, "open_gate_serial", side_effect=OSError("unavailable")), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertFalse(gate.stop_discovery("test-port", False))
        self.assertIn("STOP_UNCONFIRMED", output.getvalue())
        self.assertNotIn("STOPPED", output.getvalue())


if __name__ == "__main__":
    unittest.main()
