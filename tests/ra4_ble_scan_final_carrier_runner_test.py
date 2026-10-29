#!/usr/bin/env python3
"""Verify CDC-only BLE scan never falls back to the relay during cleanup."""
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import run_ra4_arduino_ble_scan_smoke as runner


class FinalCarrierRunnerTest(unittest.TestCase):
    def args(self):
        return SimpleNamespace(final_carrier=True, bridge_port='/dev/ttyACM0',
                               relay_port='must-not-open', capture_ms=16000,
                               arduino_cli='arduino-cli')

    def test_capture_uses_cdc_reset_and_checks_reports(self):
        result = SimpleNamespace(returncode=0, stdout='RA4_BLE_SCAN:REPORT_COUNT=129\n', stderr='')
        with patch.object(runner.subprocess, 'run', return_value=result) as run:
            self.assertEqual(runner.run_scan_capture(self.args()), 129)
        command = run.call_args.args[0]
        self.assertEqual(Path(command[1]).name, 'reset_ra4_samba_final_carrier.py')
        self.assertNotIn('--relay-port', command)
        self.assertIn('RA4_BLE_SCAN:RESULT=PASS', command)

    def test_cleanup_uses_final_carrier_and_propagates_failure(self):
        with patch.object(runner, 'run_command') as run:
            runner.restore_wifis3_scan(self.args())
        command = run.call_args.args[0]
        self.assertEqual(Path(command[1]).name, 'run_ra4_wifis3_scan_final_carrier.py')
        self.assertNotIn('--relay-port', command)
        with patch.object(runner, 'run_command', side_effect=RuntimeError('restore failed')):
            with self.assertRaisesRegex(RuntimeError, 'restore failed'):
                runner.restore_wifis3_scan(self.args())

    def test_no_report_count_fails(self):
        result = SimpleNamespace(returncode=0, stdout='', stderr='')
        with patch.object(runner.subprocess, 'run', return_value=result):
            with self.assertRaisesRegex(RuntimeError, 'report count'):
                runner.run_scan_capture(self.args())

    def test_zero_reports_fails(self):
        result = SimpleNamespace(returncode=0, stdout='RA4_BLE_SCAN:REPORT_COUNT=0\n', stderr='')
        with patch.object(runner.subprocess, 'run', return_value=result):
            with self.assertRaisesRegex(RuntimeError, 'any advertising reports'):
                runner.run_scan_capture(self.args())


if __name__ == '__main__':
    unittest.main()
