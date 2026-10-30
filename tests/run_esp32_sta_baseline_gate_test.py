#!/usr/bin/env python3
"""Unit tests for non-secret ESP32-S3 STA baseline helper logic."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


ROOT_DIR = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT_DIR / "tools" / "run_esp32_sta_baseline_gate.py"
SPEC = importlib.util.spec_from_file_location("esp32_sta_baseline_gate", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


class Esp32StaBaselineGateTests(unittest.TestCase):
    def test_beginsta_request_quotes_arguments(self) -> None:
        request = GATE.build_beginsta_request("fixture,ssid", 'p"a\\ss')
        self.assertEqual(
            request,
            b'AT+BEGINSTA="fixture,ssid","p\\"a\\\\ss"\r\n',
        )

    def test_invalid_password_never_matches_source_password(self) -> None:
        self.assertEqual(GATE.make_invalid_password("fixture"), "fixture!")
        self.assertEqual(GATE.make_invalid_password("x" * 64), "x" * 63 + "!")
        self.assertEqual(GATE.make_invalid_password("x" * 63 + "!"), "x" * 63 + "?")

    def test_response_parsers_reject_malformed_values(self) -> None:
        self.assertEqual(GATE.parse_status(b"+GETSTATUS: 3\r\nOK\r\n"), 3)
        self.assertTrue(GATE.parse_reconnect(b"+RECONNECT: 1\r\nOK\r\n"))
        self.assertFalse(GATE.parse_reconnect(b"+RECONNECT: 0\r\nOK\r\n"))
        self.assertEqual(
            GATE.parse_firmware(b"+FWVERSION: 0.2.0\r\nOK\r\n"),
            "0.2.0",
        )
        GATE.validate_beginsta(b"+BEGINSTA: 2\r\nOK\r\n")
        with self.assertRaises(RuntimeError):
            GATE.parse_status(b"+GETSTATUS: 256\r\nOK\r\n")
        with self.assertRaises(RuntimeError):
            GATE.parse_reconnect(b"+RECONNECT: 2\r\nOK\r\n")
        with self.assertRaises(RuntimeError):
            GATE.parse_firmware(b"+FWVERSION: 0.2\r\nOK\r\n")
        with self.assertRaises(RuntimeError):
            GATE.validate_beginsta(b"+BEGINSTA: 2\r\n\r\nOK\r\n")


if __name__ == "__main__":
    unittest.main()
