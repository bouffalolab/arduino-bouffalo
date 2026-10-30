#!/usr/bin/env python3
"""Unit tests for private ESP32-S3 SoftAP controller helpers."""

from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import tempfile
import unittest


ROOT_DIR = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT_DIR / "tools" / "esp32_softap_controller.py"
SPEC = importlib.util.spec_from_file_location("esp32_softap_controller", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
CONTROLLER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CONTROLLER)


class Esp32SoftApControllerTests(unittest.TestCase):
    def test_generated_credentials_fit_softap_limits(self) -> None:
        ssid, password = CONTROLLER.generate_credentials()
        request = CONTROLLER.softap.build_softap_start_request(ssid, password, 1)
        self.assertTrue(request.startswith(b'AT+BEGINSOFTAP="uno-r4-'))
        self.assertGreaterEqual(len(password), 8)

    def test_private_credentials_are_mode_600_and_removable(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "private.env"
            CONTROLLER.write_private_credentials(path, "fixture-ap", "fixture-password")
            self.assertEqual(os.stat(path).st_mode & 0o777, 0o600)
            self.assertEqual(
                path.read_text(encoding="ascii"),
                "BL616CL_STA_SSID=fixture-ap\n"
                "BL616CL_STA_PASSWORD=fixture-password\n",
            )
            CONTROLLER.remove_private_file(path)
            self.assertFalse(path.exists())

    def test_control_protocol_rejects_ambiguous_requests(self) -> None:
        self.assertEqual(CONTROLLER.parse_control_request(b"outage\n"), "outage")
        self.assertEqual(CONTROLLER.parse_control_request(b"recovery\n"), "recovery")
        self.assertEqual(CONTROLLER.parse_control_request(b"shutdown\n"), "shutdown")
        with self.assertRaises(RuntimeError):
            CONTROLLER.parse_control_request(b"outage")


if __name__ == "__main__":
    unittest.main()
