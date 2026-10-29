#!/usr/bin/env python3
"""Unit tests for credential-redacted ESP32-S3 SoftAP smoke helpers."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


ROOT_DIR = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT_DIR / "tools" / "run_esp32_softap_smoke.py"
SPEC = importlib.util.spec_from_file_location("esp32_softap_smoke", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
SMOKE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SMOKE)


class Esp32SoftApSmokeTests(unittest.TestCase):
    def test_start_request_quotes_and_bounds_arguments(self) -> None:
        self.assertEqual(
            SMOKE.build_softap_start_request('fixture,"ssid"', "fixture-password", 6),
            b'AT+BEGINSOFTAP="fixture,\\"ssid\\"","fixture-password",6,0,1\r\n',
        )
        with self.assertRaises(RuntimeError):
            SMOKE.build_softap_start_request("", "fixture-password", 1)
        with self.assertRaises(RuntimeError):
            SMOKE.build_softap_start_request("fixture", "short", 1)
        with self.assertRaises(RuntimeError):
            SMOKE.build_softap_start_request("fixture", "fixture-password", 14)

    def test_start_response_requires_successful_exact_shape(self) -> None:
        SMOKE.require_softap_start_response(b"+BEGINSOFTAP: 1\r\nOK\r\n")
        with self.assertRaises(RuntimeError):
            SMOKE.require_softap_start_response(b"+BEGINSOFTAP: 0\r\nOK\r\n")
        with self.assertRaises(RuntimeError):
            SMOKE.require_softap_start_response(b"+BEGINSOFTAP: 1\r\n\r\nOK\r\n")


if __name__ == "__main__":
    unittest.main()
