#!/usr/bin/env python3
"""Unit tests for credential-safe STA association gate helpers."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


ROOT_DIR = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT_DIR / "tools" / "run_ra4_sta_association_gate.py"
SPEC = importlib.util.spec_from_file_location("ra4_sta_association_gate", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


class StaAssociationGateTests(unittest.TestCase):
    def test_quote_at_argument_escapes_parser_metacharacters(self) -> None:
        self.assertEqual(GATE.quote_at_argument('ap,"west"\\lab'), '"ap,\\"west\\"\\\\lab"')

    def test_build_beginsta_request_quotes_both_secret_arguments(self) -> None:
        request = GATE.build_beginsta_request("fixture,ssid", 'p"a\\ss')
        self.assertEqual(
            request,
            b'AT+BEGINSTA="fixture,ssid","p\\"a\\\\ss"\r\n',
        )

    def test_open_network_omits_password_argument(self) -> None:
        self.assertEqual(
            GATE.build_beginsta_request("fixture-open", None),
            b'AT+BEGINSTA="fixture-open"\r\n',
        )

    def test_credential_validation_rejects_unsafe_or_invalid_values(self) -> None:
        self.assertEqual(
            GATE.require_ascii_credential("BL616CL_STA_SSID", "fixture", 32, True),
            "fixture",
        )
        with self.assertRaisesRegex(RuntimeError, "must be set"):
            GATE.require_ascii_credential("BL616CL_STA_SSID", None, 32, True)
        with self.assertRaisesRegex(RuntimeError, "must not be empty"):
            GATE.require_ascii_credential("BL616CL_STA_PASSWORD", "", 64, False)
        with self.assertRaisesRegex(RuntimeError, "ASCII"):
            GATE.require_ascii_credential("BL616CL_STA_SSID", "网络", 32, True)
        with self.assertRaisesRegex(RuntimeError, "forbidden"):
            GATE.require_ascii_credential("BL616CL_STA_SSID", "bad\rssid", 32, True)
        with self.assertRaisesRegex(RuntimeError, "exceeds"):
            GATE.require_ascii_credential("BL616CL_STA_SSID", "x" * 33, 32, True)

    def test_dynamic_frames_accept_only_valid_values(self) -> None:
        self.assertTrue(GATE.parse_reconnect_response(b"+RECONNECT: 1\r\nOK\r\n"))
        self.assertFalse(GATE.parse_reconnect_response(b"+RECONNECT: 0\r\nOK\r\n"))
        self.assertEqual(GATE.parse_status_response(b"+GETSTATUS: 3\r\nOK\r\n"), 3)
        GATE.validate_beginsta_response(b"+BEGINSTA: 2\r\nOK\r\n")
        with self.assertRaises(RuntimeError):
            GATE.parse_reconnect_response(b"+RECONNECT: 2\r\nOK\r\n")
        with self.assertRaises(RuntimeError):
            GATE.parse_status_response(b"+GETSTATUS: 256\r\nOK\r\n")
        with self.assertRaises(RuntimeError):
            GATE.validate_beginsta_response(b"+BEGINSTA: 2\r\n\r\nOK\r\n")

    def test_poll_for_status_returns_deduplicated_observed_sequence(self) -> None:
        responses = iter(
            [
                b"+GETSTATUS: 2\r\nOK\r\n",
                b"+GETSTATUS: 2\r\nOK\r\n",
                b"+GETSTATUS: 3\r\nOK\r\n",
            ]
        )
        original_transact = GATE.transact
        original_monotonic = GATE.time.monotonic
        original_sleep = GATE.time.sleep
        try:
            GATE.transact = lambda *args, **kwargs: next(responses)
            GATE.time.monotonic = iter([1.0, 1.0, 1.1, 1.1, 1.2]).__next__
            GATE.time.sleep = lambda _: None
            elapsed_ms, statuses = GATE.poll_for_status(
                object(),
                object(),
                expected_status=3,
                timeout_seconds=2.0,
                response_timeout_seconds=0.1,
                poll_interval_seconds=0.01,
            )
        finally:
            GATE.transact = original_transact
            GATE.time.monotonic = original_monotonic
            GATE.time.sleep = original_sleep
        self.assertAlmostEqual(elapsed_ms, 200.0)
        self.assertEqual(statuses, [2, 3])

    def test_poll_for_status_keeps_observed_sequence_on_timeout(self) -> None:
        responses = iter(
            [
                b"+GETSTATUS: 0\r\nOK\r\n",
                b"+GETSTATUS: 0\r\nOK\r\n",
            ]
        )
        original_transact = GATE.transact
        original_monotonic = GATE.time.monotonic
        original_sleep = GATE.time.sleep
        try:
            GATE.transact = lambda *args, **kwargs: next(responses)
            GATE.time.monotonic = iter([1.0, 1.0, 1.1, 2.1]).__next__
            GATE.time.sleep = lambda _: None
            elapsed_ms, statuses = GATE.poll_for_status(
                object(),
                object(),
                expected_status=3,
                timeout_seconds=1.0,
                response_timeout_seconds=0.1,
                poll_interval_seconds=0.01,
            )
        finally:
            GATE.transact = original_transact
            GATE.time.monotonic = original_monotonic
            GATE.time.sleep = original_sleep
        self.assertIsNone(elapsed_ms)
        self.assertEqual(statuses, [0])


if __name__ == "__main__":
    unittest.main()
