#!/usr/bin/env python3
"""Offline regression tests for the ESP32-S3 dynamic AT capture tool."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest


ROOT_DIR = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT_DIR / "tools" / "capture_esp32_at_transcript.py"
STATEFUL_CASE_FILE = ROOT_DIR / "tests" / "at_golden" / "cases" / "stateful_wifi.json"
BLE_HCI_CASE_FILE = ROOT_DIR / "tests" / "at_golden" / "cases" / "dynamic_ble_hci.json"
SPEC = importlib.util.spec_from_file_location("capture_esp32_at_transcript", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
capture = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = capture
SPEC.loader.exec_module(capture)


class FakeSerialPort:
    def __init__(self, response_chunks: list[bytes]) -> None:
        self._response_chunks = response_chunks
        self.writes: list[bytes] = []

    @property
    def in_waiting(self) -> int:
        if not self.writes or not self._response_chunks:
            return 0
        return len(self._response_chunks[0])

    def read(self, _count: int) -> bytes:
        return self._response_chunks.pop(0)

    def write(self, data: bytes) -> int:
        self.writes.append(data)
        return len(data)

    def flush(self) -> None:
        pass


class CaptureTranscriptTests(unittest.TestCase):
    def test_checked_in_cases_validate(self) -> None:
        cases = capture.load_cases(capture.DEFAULT_CASE_FILE)
        self.assertEqual(len(cases), 9)
        self.assertEqual(cases["wifi_scan"].request, b"AT+WIFISCAN\r\n")
        self.assertEqual(cases["wifi_scan"].follow_ups, (b"AT+GETSTATUS?\r\n",))
        self.assertNotIn("wifi_disconnect", cases)
        self.assertEqual(cases["autoconnect_write_enable"].state_change, "none")
        self.assertEqual(cases["autoconnect_write_disable"].state_change, "none")

    def test_stateful_cases_require_preflight_and_manual_recovery(self) -> None:
        cases = capture.load_cases(STATEFUL_CASE_FILE)
        self.assertEqual(
            set(cases),
            {
                "wifi_mode_write_null",
                "wifi_mode_write_sta",
                "reconnect_write_disable",
                "reconnect_write_enable",
                "wifi_disconnect_run",
            },
        )
        disconnect = cases["wifi_disconnect_run"]
        self.assertEqual(disconnect.state_change, "temporary")
        self.assertEqual(
            disconnect.pre_state_requests,
            (
                b"AT+GETSTATUS?\r\n",
                b"AT+WIFIMODE?\r\n",
                b"AT+RECONNECT?\r\n",
                b"AT+AUTOCONNECT?\r\n",
            ),
        )
        self.assertIsNotNone(disconnect.recovery)
        assert disconnect.recovery is not None
        self.assertFalse(disconnect.recovery.automatic_restore)
        self.assertTrue(disconnect.recovery.operator_actions)

    def test_ble_hci_case_preserves_raw_payload_and_cleanup_order(self) -> None:
        cases = capture.load_cases(BLE_HCI_CASE_FILE)
        case = cases["hci_reset_round_trip"]

        self.assertEqual(case.state_change, "temporary")
        self.assertEqual(case.pre_state_requests, (b"AT+FWVERSION?\r\n",))
        self.assertEqual(
            case.pre_state_expected_responses,
            (b"+FWVERSION: 0.6.0\r\nOK\r\n",),
        )
        self.assertEqual(
            [step.phase for step in case.steps],
            [
                "hci_begin",
                "hci_write_reset",
                "hci_wait",
                "hci_available",
                "hci_read",
                "hci_end",
            ],
        )
        self.assertEqual(
            case.steps[1].wire_request,
            b"AT+HCIWRITE=4\r\n\x01\x03\x0c\x00",
        )
        self.assertEqual(case.steps[2].request, b"AT+HCIWAIT=1000\n\r")
        self.assertEqual(case.steps[-1].request, b"AT+HCIEND\r\n")

        cleanup = cases["hci_end_after_ra4_wait_smoke"]
        self.assertEqual(
            [step.phase for step in cleanup.steps],
            ["hci_end_after_ra4_wait_smoke"],
        )
        self.assertEqual(cleanup.steps[0].request, b"AT+HCIEND\r\n")

        wait_250 = cases["hci_wait_250_round_trip"]
        self.assertEqual(
            [step.phase for step in wait_250.steps],
            [
                "hci_begin",
                "hci_wait_250",
                "hci_available_after_wait",
                "hci_end",
            ],
        )
        self.assertEqual(wait_250.steps[1].request, b"AT+HCIWAIT=250\n\r")
        self.assertEqual(wait_250.steps[2].request, b"AT+HCIAVAILABLE?\r\n")
        self.assertEqual(wait_250.completion.timeout_ms, 15000)

    def test_preflight_record_must_match_the_selected_stateful_case(self) -> None:
        case = capture.load_cases(STATEFUL_CASE_FILE)["reconnect_write_disable"]
        exchanges = [
            {
                "phase": "pre_state",
                "request": {"hex": request.hex()},
                "timed_out": False,
            }
            for request in case.pre_state_requests
        ]
        record = {
            "schema_version": capture.CAPTURE_SCHEMA_VERSION,
            "case": {"id": case.case_id},
            "execution": {"mode": "preflight"},
            "exchanges": exchanges,
        }

        import json
        import tempfile

        with self.subTest("valid"):
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "preflight.json"
                path.write_text(json.dumps(record), encoding="utf-8")
                digest = capture.read_preflight_record(case, path)
                self.assertEqual(len(digest), 64)

        with self.subTest("wrong-case"):
            record["case"]["id"] = "wifi_mode_write_sta"
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "preflight.json"
                path.write_text(json.dumps(record), encoding="utf-8")
                with self.assertRaises(capture.CaseError):
                    capture.read_preflight_record(case, path)

    def test_preflight_record_checks_required_baseline_response(self) -> None:
        case = capture.load_cases(BLE_HCI_CASE_FILE)["hci_reset_round_trip"]
        expected_request = case.pre_state_requests[0]
        expected_response = case.pre_state_expected_responses[0]
        record = {
            "schema_version": capture.CAPTURE_SCHEMA_VERSION,
            "case": {"id": case.case_id},
            "execution": {"mode": "preflight"},
            "exchanges": [
                {
                    "phase": "pre_state",
                    "request": {"hex": expected_request.hex()},
                    "response": {"hex": expected_response.hex()},
                    "timed_out": False,
                }
            ],
        }

        import json
        import tempfile

        with self.subTest("matching-response"):
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "preflight.json"
                path.write_text(json.dumps(record), encoding="utf-8")
                self.assertEqual(len(capture.read_preflight_record(case, path)), 64)

        with self.subTest("wrong-response"):
            record["exchanges"][0]["response"]["hex"] = b"ERROR\r\n".hex()
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "preflight.json"
                path.write_text(json.dumps(record), encoding="utf-8")
                with self.assertRaises(capture.CaseError):
                    capture.read_preflight_record(case, path)

    def test_capture_keeps_bytes_after_terminal_during_quiet_window(self) -> None:
        port = FakeSerialPort([b"OK\r\n", b"tail"])
        completion = capture.Completion(
            terminals=(b"OK\r\n", b"ERROR\r\n"),
            timeout_ms=100,
            quiet_ms=0,
        )

        exchange = capture.capture_request(port, b"AT\r\n", completion)

        self.assertEqual(port.writes, [b"AT\r\n"])
        self.assertEqual(exchange["terminal"], "OK\\r\\n")
        self.assertFalse(exchange["timed_out"])
        self.assertEqual(exchange["response"]["hex"], b"OK\r\ntail".hex())

    def test_capture_step_writes_ascii_header_and_raw_payload_together(self) -> None:
        port = FakeSerialPort([b"OK\r\n"])
        completion = capture.Completion(
            terminals=(b"OK\r\n", b"ERROR\r\n"),
            timeout_ms=100,
            quiet_ms=0,
        )
        step = capture.CaptureStep(
            phase="hci_write_reset",
            request=b"AT+HCIWRITE=4\r\n",
            raw_payload=b"\x01\x03\x0c\x00",
        )

        exchange = capture.capture_exchange(port, step.wire_request, completion, step.phase)

        self.assertEqual(port.writes, [b"AT+HCIWRITE=4\r\n\x01\x03\x0c\x00"])
        self.assertEqual(exchange["phase"], "hci_write_reset")
        self.assertEqual(exchange["request"]["hex"], "41542b48434957524954453d340d0a01030c00")

    def test_steps_reject_malformed_raw_payload(self) -> None:
        raw_case = {
            "id": "bad_raw_payload",
            "classification": "hardware-capture-required",
            "summary": "Malformed payload validation.",
            "preconditions": [],
            "state_change": "none",
            "steps": [
                {
                    "phase": "write",
                    "request_ascii": "AT+HCIWRITE=1\r\n",
                    "raw_payload_hex": "0",
                }
            ],
            "completion": {
                "type": "terminal-line",
                "terminal_ascii": ["OK\r\n"],
                "timeout_ms": 100,
                "quiet_ms": 0,
            },
        }

        with self.assertRaises(capture.CaseError):
            capture.parse_case(raw_case)

    def test_binary_encoding_is_lossless(self) -> None:
        encoded = capture.encode_bytes(b"\x00A\r\n\xff")
        self.assertEqual(encoded["hex"], "00410d0aff")
        self.assertEqual(encoded["printable_ascii"], "\\x00A\\x0d\\x0a\\xff")


if __name__ == "__main__":
    unittest.main()
