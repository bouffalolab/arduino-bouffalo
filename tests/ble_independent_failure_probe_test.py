from pathlib import Path
import contextlib
import io
import json
import sys
import tempfile
import unittest
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import ble_independent_failure_probe as probe


class FailureProbeTest(unittest.TestCase):
    def test_detached_baseline_repair_preserves_bl(self):
        with tempfile.TemporaryDirectory() as temporary:
            sys_root = Path(temporary)
            controller = sys_root / "devices/platform/vhci_hcd.0"
            controller.mkdir(parents=True)
            (controller / "status").write_text(
                "hub port sta spd dev sockfd local_busid\n"
                "hs 0000 006 003 00030001 000003 1-1\n"
                "hs 0001 006 002 00030003 000003 1-2\n")
            for device, serial in (("1-1", "BL616CL-PROVISIONAL"), ("1-2", "TESTESP000001")):
                directory = sys_root / "bus/usb/devices" / device
                directory.mkdir(parents=True)
                for name, value in (("idVendor", "2341"), ("idProduct", "1002"), ("serial", serial)):
                    (directory / name).write_text(value)
            state = {"Devices": [{"BusId": "3-3", "IsWslAttached": False,
                                 "InstanceId": "USB\\VID_2341&PID_1002\\TESTESP000001"}]}
            with patch.object(probe.subprocess, "run") as run, \
                    patch.object(probe, "resolve_baseline_esp32_port"), \
                    contextlib.redirect_stdout(io.StringIO()):
                run.return_value.stdout = json.dumps(state).encode()
                probe.prepare_baseline_usb(sys_root)
            self.assertEqual(len(run.call_args_list), 3)
            self.assertEqual(run.call_args_list[1].args[0][-1], f"echo 1 > {controller / 'detach'}")
            self.assertEqual(run.call_args_list[2].args[0][4], "3-3")

    def test_attached_baseline_is_not_detached(self):
        state = {"Devices": [{"BusId": "3-3", "IsWslAttached": True,
                             "InstanceId": "USB\\VID_2341&PID_1002\\TESTESP000001"}]}
        with patch.object(probe.subprocess, "run") as run:
            run.return_value.stdout = json.dumps(state).encode()
            probe.prepare_baseline_usb()
            self.assertEqual(run.call_count, 1)

    def test_unknown_baseline_is_not_modified(self):
        state = {"Devices": [{"BusId": "3-3", "IsWslAttached": False,
                             "InstanceId": "USB\\VID_2341&PID_1002\\UNKNOWN"}]}
        with patch.object(probe.subprocess, "run") as run:
            run.return_value.stdout = json.dumps(state).encode()
            with self.assertRaises(RuntimeError):
                probe.prepare_baseline_usb()
            self.assertEqual(run.call_count, 1)

    def exercise(self, lines, prepared=False):
        link = MagicMock()
        link.__enter__.return_value = link
        link.readline.side_effect = [("RA4_BLE_LONG_GATT_CENTRAL:" + row + "\r\n").encode()
                                     for row in lines]
        with tempfile.TemporaryDirectory() as temporary:
            image = Path(temporary) / "probe.bin"
            image.write_bytes(b"fixture")
            with patch.object(probe, "resolve_baseline_esp32_port", return_value="baseline"), \
                    patch.object(probe, "prepare_baseline_usb"), \
                    patch.object(probe.baseline, "require_port_identity"), \
                    patch.object(probe.baseline, "open_serial", return_value=link), \
                    patch.object(probe.baseline, "restore_scan_smoke") as restore, \
                    patch.object(probe.subprocess, "run") as upload, \
                    patch.object(probe, "run") as run, contextlib.redirect_stdout(io.StringIO()) as output:
                error = None
                try:
                    probe.run_probe(image, 4, prepared_port="baseline" if prepared else None)
                except RuntimeError as caught:
                    error = caught
        restore.assert_called_once()
        if prepared:
            run.assert_not_called()
            upload.assert_not_called()
        else:
            run.assert_called_once_with(probe.CLI, "board", "list")
            upload.assert_called_once()
            self.assertIn("baseline", upload.call_args.args[0])
            self.assertEqual(upload.call_args.kwargs["timeout"], 90)
        link.write.assert_called_once_with(b"S\x04")
        return error, output.getvalue()

    def test_read_only_probe_requires_complete_evidence(self):
        error, output = self.exercise(["WAIT_START", "CONNECT=PASS", "DISCOVER=PASS",
                                      "PROBE_READ_PASS=4", "SESSION_PASS=1", "RESULT=PASS"])
        self.assertIsNone(error)
        self.assertIn("READ_PASS", output)
        self.assertIn("BASELINE_RESTORED", output)

    def test_preloaded_probe_does_not_upload_after_failure(self):
        error, output = self.exercise(["WAIT_START", "CONNECT=PASS", "DISCOVER=PASS",
                                      "PROBE_READ_PASS=4", "SESSION_PASS=1", "RESULT=PASS"],
                                     prepared=True)
        self.assertIsNone(error)
        self.assertIn("READ_PASS", output)

    def test_failure_still_restores_baseline_and_does_not_pass(self):
        error, output = self.exercise(["WAIT_START", "RESULT=FAIL:SCAN_TIMEOUT"])
        self.assertIsNotNone(error)
        self.assertNotIn(":READ_PASS", output)

    def test_partial_evidence_is_not_a_pass(self):
        error, output = self.exercise(["WAIT_START", "RESULT=PASS"])
        self.assertIsNotNone(error)
        self.assertNotIn(":READ_PASS", output)


if __name__ == "__main__":
    unittest.main()
